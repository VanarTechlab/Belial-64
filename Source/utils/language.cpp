#include "utils/language.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>

#else
#include <SDL.h>

#ifdef USE_SDL1
#include "utils/sdl2_to_1_2_backports.h"
#endif
#endif

#include <ankerl/unordered_dense.h>
#include <function_ref.hpp>

#include "engine/assets.hpp"
#include "options.h"
#include "utils/algorithm/container.hpp"
#include "utils/endian_swap.hpp"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/paths.h"
#include "utils/string_view_hash.hpp"
#include "utils/str_cat.hpp"
#include <algorithm>

#define MO_MAGIC 0x950412de

std::string forceLocale;

namespace {

using namespace devilution;

// Translations normally come in ".gmo" files.
// We also support ".mo" because that is what poedit generates
// and what translators use to test their work.
constexpr std::array<const char *, 2> Extensions { ".mo", ".gmo" };

std::unique_ptr<char[]> translationKeys;
std::unique_ptr<char[]> translationValues;

using TranslationRef = uint32_t;

std::vector<ankerl::unordered_dense::map<const char *, TranslationRef, StringViewHash, StringViewEquals>> translation = { {}, {} };

constexpr uint32_t TranslationRefOffsetBits = 19;
constexpr uint32_t TranslationRefSizeBits = 32 - TranslationRefOffsetBits; // 13
constexpr uint32_t TranslationRefSizeMask = (1 << TranslationRefSizeBits) - 1;

TranslationRef EncodeTranslationRef(uint32_t offset, uint32_t size)
{
	return (offset << TranslationRefSizeBits) | size;
}

std::string_view GetTranslation(TranslationRef ref)
{
	return { &translationValues[ref >> TranslationRefSizeBits], ref & TranslationRefSizeMask };
}

#ifdef __N64__
// f2v74: la traduccion vive en el cartucho; en la RAM solo un indice (hash de la clave -> posicion) y
// los textos que se van usando (antes ~300 KiB con el archivo entero en la RAM).
extern "C" uint32_t n64_lang_to_cart(const char *path);
extern "C" void n64_lang_read(void *dst, uint32_t off, uint32_t len);
struct N64LangEntry {
	uint64_t hash;
	uint32_t off;
	uint16_t len;
	uint8_t form;
};
std::vector<N64LangEntry> n64Lang;
std::vector<char *> n64LangCache;
std::vector<char *> n64LangHeap;
constexpr size_t N64LangArenaSize = 48 * 1024;
std::unique_ptr<char[]> n64LangArena;
size_t n64LangArenaUsed = 0;
bool n64LangOn = false;
uint64_t N64Hash(std::string_view s)
{
	uint64_t h = 1469598103934665603ULL;
	for (const char c : s) {
		h ^= static_cast<uint8_t>(c);
		h *= 1099511628211ULL;
	}
	return h;
}
void N64LangClear()
{
	for (char *p : n64LangHeap)
		delete[] p;
	n64LangHeap = {};
	n64LangCache = {};
	n64LangArena = nullptr;
	n64LangArenaUsed = 0;
	n64Lang = {};
	n64LangOn = false;
}
bool N64Find(std::string_view key, unsigned form, std::string_view &out)
{
	const uint64_t h = N64Hash(key);
	auto it = std::lower_bound(n64Lang.begin(), n64Lang.end(), h, [](const N64LangEntry &e, uint64_t v) { return e.hash < v; });
	for (; it != n64Lang.end() && it->hash == h; ++it) {
		if (it->form != form)
			continue;
		const size_t idx = static_cast<size_t>(it - n64Lang.begin());
		if (n64LangCache[idx] == nullptr) {
			// f2v75: los textos van a un bloque propio (pedido una vez al cargar el idioma) para no
			// trocear la memoria con miles de trozos pequenos
			char *p;
			if (n64LangArena != nullptr && n64LangArenaUsed + it->len + 1 <= N64LangArenaSize) {
				p = n64LangArena.get() + n64LangArenaUsed;
				n64LangArenaUsed += it->len + 1;
			} else {
				p = new char[it->len + 1];
				n64LangHeap.push_back(p);
			}
			n64_lang_read(p, it->off, it->len);
			p[it->len] = '\0';
			n64LangCache[idx] = p;
		}
		out = std::string_view(n64LangCache[idx], it->len);
		return true;
	}
	return false;
}
#endif

} // namespace

namespace {

struct MoHead {
	uint32_t magic;
	struct {
		uint16_t major;
		uint16_t minor;
	} revision;

	uint32_t nbMappings;
	uint32_t srcOffset;
	uint32_t dstOffset;
};

void SwapLE(MoHead &head)
{
	head.magic = Swap32LE(head.magic);
	head.revision.major = Swap16LE(head.revision.major);
	head.revision.minor = Swap16LE(head.revision.minor);
	head.nbMappings = Swap32LE(head.nbMappings);
	head.srcOffset = Swap32LE(head.srcOffset);
	head.dstOffset = Swap32LE(head.dstOffset);
}

struct MoEntry {
	uint32_t length;
	uint32_t offset;
};

void SwapLE(MoEntry &entry)
{
	entry.length = Swap32LE(entry.length);
	entry.offset = Swap32LE(entry.offset);
}

std::string_view TrimLeft(std::string_view str)
{
	str.remove_prefix(std::min(str.find_first_not_of(" \t"), str.size()));
	return str;
}

std::string_view TrimRight(std::string_view str)
{
	str.remove_suffix(str.size() - (str.find_last_not_of(" \t") + 1));
	return str;
}

/** plural=(n != 1); */
int PluralIfNotOne(int n)
{
	return n != 1 ? 1 : 0;
}

// English, Danish, Spanish, Italian, Swedish
unsigned PluralForms = 2;
tl::function_ref<int(int n)> GetLocalPluralId = PluralIfNotOne;

/**
 * Match plural=(n != 1);"
 */
void SetPluralForm(std::string_view expression)
{
	const std::string_view key = "plural=";
	const std::string_view::size_type keyPos = expression.find(key);
	if (keyPos == std::string_view::npos)
		return;
	expression.remove_prefix(keyPos + key.size());

	const std::string_view::size_type semicolonPos = expression.find(';');
	if (semicolonPos != std::string_view::npos) {
		expression.remove_suffix(expression.size() - semicolonPos);
	}

	expression = TrimLeft(TrimRight(expression));

	// ko, zh_CN, zh_TW
	if (expression == "0") {
		GetLocalPluralId = [](int /*n*/) -> int { return 0; };
		return;
	}

	// en, bg, da, de, es, fi, it, sv
	if (expression == "(n != 1)") {
		GetLocalPluralId = PluralIfNotOne;
		return;
	}

	// fr, pt_BR
	if (expression == "(n > 1)") {
		GetLocalPluralId = [](int n) -> int { return n > 1 ? 1 : 0; };
		return;
	}

	// be, hr, ru
	if (expression == "(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<12 || n%100>14) ? 1 : 2)") {
		GetLocalPluralId = [](int n) -> int {
			if (n % 10 == 1 && n % 100 != 11)
				return 0;
			if (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14))
				return 1;
			return 2;
		};
		return;
	}

	// pl
	if (expression == "(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<12 || n%100>14) ? 1 : 2)") {
		GetLocalPluralId = [](int n) -> int {
			if (n == 1)
				return 0;
			if (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14))
				return 1;
			return 2;
		};
		return;
	}

	// ro
	if (expression == "(n==1 ? 0 : n==0 || (n!=1 && n%100>=1 && n%100<=19) ? 1 : 2)") {
		GetLocalPluralId = [](int n) -> int {
			if (n == 1)
				return 0;
			if (n == 0 || (n != 1 && n % 100 >= 1 && n % 100 <= 19))
				return 1;
			return 2;
		};
		return;
	}

	// cs
	if (expression == "(n==1) ? 0 : (n>=2 && n<=4) ? 1 : 2") {
		GetLocalPluralId = [](int n) -> int {
			if (n == 1)
				return 0;
			if (n >= 2 && n <= 4)
				return 1;
			return 2;
		};
		return;
	}

	LogError("Unknown plural expression: '{}'", expression);
}

/**
 * Parse "nplurals=2;"
 */
void ParsePluralForms(std::string_view string)
{
	const std::string_view pluralsKey = "nplurals";
	const std::string_view::size_type pluralsPos = string.find(pluralsKey);
	if (pluralsPos == std::string_view::npos)
		return;
	string.remove_prefix(pluralsPos + pluralsKey.size());

	const std::string_view::size_type eqPos = string.find('=');
	if (eqPos == std::string_view::npos)
		return;

	const std::string_view value = string.substr(eqPos + 1);
	if (value.empty() || value[0] < '0')
		return;

	const unsigned nplurals = value[0] - '0';
	if (nplurals == 0)
		return;

	PluralForms = nplurals;

	SetPluralForm(value);
}

void ParseMetadata(std::string_view metadata)
{
	std::string_view::size_type delim;

	while (!metadata.empty() && ((delim = metadata.find(':')) != std::string_view::npos)) {
		const std::string_view key = TrimLeft(std::string_view(metadata.data(), delim));
		std::string_view val = TrimLeft(std::string_view(metadata.data() + delim + 1, metadata.size() - delim - 1));

		if ((delim = val.find('\n')) != std::string_view::npos) {
			val = std::string_view(val.data(), delim);
			metadata.remove_prefix(val.data() - metadata.data() + val.size() + 1);
		} else {
			metadata.remove_prefix(metadata.size());
		}

		// Match "Plural-Forms: nplurals=2; plural=(n != 1);"
		if (key == "Plural-Forms") {
			ParsePluralForms(val);
			break;
		}
	}
}

bool ReadEntry(AssetHandle &handle, const MoEntry &e, char *result)
{
	if (!handle.seek(e.offset))
		return false;
	result[e.length] = '\0';
	return handle.read(result, e.length);
}

bool CopyData(void *dst, const std::byte *data, size_t dataSize, size_t offset, size_t length)
{
	if (offset + length > dataSize)
		return false;
	memcpy(dst, data + offset, length);
	return true;
}

bool ReadEntry(const std::byte *data, size_t dataSize, const MoEntry &e, char *result)
{
	if (!CopyData(result, data, dataSize, e.offset, e.length))
		return false;
	result[e.length] = '\0';
	return true;
}

} // namespace

std::string_view LanguageParticularTranslate(std::string_view context, std::string_view message)
{
	constexpr const char Glue = '\004';

	std::string key = std::string(context);
	key.reserve(key.size() + 1 + message.size());
	key += Glue;
	key.append(message);

#ifdef __N64__
	if (n64LangOn) {
		std::string_view r;
		return N64Find(key, 0, r) ? r : message;
	}
#endif
	auto it = translation[0].find(key.c_str());
	if (it == translation[0].end()) {
		return message;
	}

	return GetTranslation(it->second);
}

std::string_view LanguagePluralTranslate(const char *singular, std::string_view plural, int count)
{
	const int n = GetLocalPluralId(count);
#ifdef __N64__
	if (n64LangOn) {
		std::string_view r;
		if (N64Find(singular, static_cast<unsigned>(n), r))
			return r;
		return count != 1 ? plural : std::string_view(singular);
	}
#endif

	auto it = translation[n].find(singular);
	if (it == translation[n].end()) {
		if (count != 1)
			return plural;
		return singular;
	}

	return GetTranslation(it->second);
}

std::string_view LanguageTranslate(const char *key)
{
#ifdef __N64__
	if (n64LangOn) {
		std::string_view r;
		return N64Find(key, 0, r) ? r : std::string_view(key);
	}
#endif
	auto it = translation[0].find(key);
	if (it == translation[0].end()) {
		return key;
	}

	return GetTranslation(it->second);
}

bool HasTranslation(const std::string &locale)
{
	if (locale == "en") {
		// the base translation is en (really en_US). No translation file will be present for this code but we want
		//  the check to succeed to prevent further searches.
		return true;
	}

	return c_any_of(Extensions, [locale](const char *extension) {
		return FindAsset((locale + extension).c_str()).ok();
	});
}

std::string_view GetLanguageCode()
{
	if (!forceLocale.empty())
		return forceLocale;
	return *GetOptions().Language.code;
}

void LanguageInitialize()
{
	translation = { {}, {} };
	translationKeys = nullptr;
	translationValues = nullptr;
#ifdef __N64__
	forceLocale.clear(); // f2v72: un fallo anterior dejaba el ingles forzado para siempre
	N64LangClear();
#endif

	const std::string lang(GetLanguageCode());
#ifdef __N64__
	LogInfo("Idioma: {}", lang);
#endif

	if (lang == "en") {
		// English does not have a translation file.
		GetLocalPluralId = PluralIfNotOne;
		return;
	}

	if (IsSmallFontTall() && !HaveExtraFonts()) {
		UiErrorOkDialog(
		    "Missing fonts.mpq",
		    StrCat("fonts.mpq is required for locale \"",
		        GetLanguageCode(),
		        "\"\n\n"
		        "Please download fonts.mpq from:\n"
		        "github.com/diasurgical/\ndevilutionx-assets/releases"));
		forceLocale = "en";
		GetLocalPluralId = PluralIfNotOne;
		return;
	}

#ifdef __N64__
	{
		// f2v74: copiar el .gmo al cartucho y quedarse solo con un indice en la RAM
		const uint32_t t0 = SDL_GetTicks();
		const std::string full = StrCat(paths::AssetsPath(), lang, ".gmo");
		const uint32_t size = n64_lang_to_cart(full.c_str());
		MoHead head {};
		if (size >= sizeof(MoHead)) {
			n64_lang_read(&head, 0, sizeof(MoHead));
			SwapLE(head);
		}
		if (size >= sizeof(MoHead) && head.magic == MO_MAGIC && head.nbMappings > 1
		    && head.srcOffset + head.nbMappings * sizeof(MoEntry) <= size && head.dstOffset + head.nbMappings * sizeof(MoEntry) <= size) {
			std::vector<MoEntry> src(head.nbMappings);
			std::vector<MoEntry> dst(head.nbMappings);
			n64_lang_read(src.data(), head.srcOffset, head.nbMappings * sizeof(MoEntry));
			n64_lang_read(dst.data(), head.dstOffset, head.nbMappings * sizeof(MoEntry));
			for (uint32_t i = 0; i < head.nbMappings; i++) {
				SwapLE(src[i]);
				SwapLE(dst[i]);
			}
			{
				std::string meta(dst[0].length, '\0');
				n64_lang_read(meta.data(), dst[0].offset, dst[0].length);
				ParseMetadata(meta);
			}
			translation.resize(PluralForms);
			for (unsigned i = 0; i < PluralForms; i++)
				translation[i] = {};
			n64Lang.reserve(head.nbMappings + 64);
			std::string key;
			std::string value;
			for (uint32_t i = 1; i < head.nbMappings; i++) {
				if (src[i].offset + src[i].length > size || dst[i].offset + dst[i].length > size)
					continue;
				key.assign(src[i].length, '\0');
				n64_lang_read(key.data(), src[i].offset, src[i].length);
				const size_t keyEnd = key.find('\0'); // la forma plural de la clave no cuenta
				const uint64_t h = N64Hash(std::string_view(key).substr(0, keyEnd));
				value.assign(dst[i].length, '\0');
				n64_lang_read(value.data(), dst[i].offset, dst[i].length);
				size_t pos = 0;
				for (unsigned j = 0; j < PluralForms && pos <= value.size(); j++) {
					size_t end = value.find('\0', pos);
					if (end == std::string::npos)
						end = value.size();
					n64Lang.push_back({ h, dst[i].offset + static_cast<uint32_t>(pos), static_cast<uint16_t>(end - pos), static_cast<uint8_t>(j) });
					if (end >= value.size())
						break;
					pos = end + 1;
				}
			}
			std::sort(n64Lang.begin(), n64Lang.end(), [](const N64LangEntry &a, const N64LangEntry &b) { return a.hash != b.hash ? a.hash < b.hash : a.form < b.form; });
			n64Lang.shrink_to_fit();
			n64LangCache.assign(n64Lang.size(), nullptr);
			n64LangArena.reset(new char[N64LangArenaSize]);
			n64LangOn = true;
			LogInfo("Idioma: {} en el cartucho, {} textos, indice {} KiB, {} ms", lang, n64Lang.size(), n64Lang.size() * (sizeof(N64LangEntry) + sizeof(char *)) / 1024, SDL_GetTicks() - t0);
			return;
		}
		LogInfo("Idioma: no se pudo usar {} desde el cartucho; se carga en la RAM", full);
	}
#endif
	AssetHandle handle;
	const uint32_t loadTranslationsStart = SDL_GetTicks();

	std::string translationsPath;
	size_t fileSize;
	for (const char *ext : Extensions) {
		translationsPath = lang + ext;
		handle = OpenAsset(translationsPath.c_str(), fileSize);
		if (handle.ok())
			break;
	}
	if (!handle.ok()) {
#ifdef __N64__
		LogInfo("Idioma: no se encontro {}.gmo en assets (se queda en ingles)", lang);
#endif
		// Reset to English, which is always available:
		forceLocale = "en";
		GetLocalPluralId = PluralIfNotOne;
		return;
	}

#if defined(UNPACKED_MPQS) && defined(__N64__)
	const bool readWholeFile = true; // f2v73: leida de golpe (con miles de lecturas pequenas tardaba 6,5 s)
#elif defined(UNPACKED_MPQS)
	const bool readWholeFile = false;
#else
#ifdef USE_SDL3
	// In SDL3, we don't have a way to check if the handle is to an MPQ file.
	// Assume that it is.
	const bool readWholeFile = true;
#else
	// If reading from an MPQ, it is much faster to
	// load the whole file instead of seeking.
	const bool readWholeFile = handle.handle->type == SDL_RWOPS_UNKNOWN;
#endif
#endif

	std::unique_ptr<std::byte[]> data;
	if (readWholeFile) {
		data.reset(new std::byte[fileSize]);
		if (!handle.read(data.get(), fileSize))
			return;
		handle = {};
	}

	// Read header and do sanity checks
	MoHead head;
	if (readWholeFile
	        ? !CopyData(&head, data.get(), fileSize, 0, sizeof(MoHead))
	        : !handle.read(&head, sizeof(MoHead))) {
		return;
	}
	SwapLE(head);

	if (head.magic != MO_MAGIC) {
		return; // not a MO file
	}

	if (head.revision.major > 1 || head.revision.minor > 1) {
		return; // unsupported revision
	}

	// Read entries of source strings
	const std::unique_ptr<MoEntry[]> src { new MoEntry[head.nbMappings] };
	if (readWholeFile
	        ? !CopyData(src.get(), data.get(), fileSize, head.srcOffset, head.nbMappings * sizeof(MoEntry))
	        : !handle.seek(head.srcOffset) || !handle.read(src.get(), head.nbMappings * sizeof(MoEntry))) {
		return;
	}
	for (size_t i = 0; i < head.nbMappings; ++i) {
		SwapLE(src[i]);
	}

	// Read entries of target strings
	const std::unique_ptr<MoEntry[]> dst { new MoEntry[head.nbMappings] };
	if (readWholeFile
	        ? !CopyData(dst.get(), data.get(), fileSize, head.dstOffset, head.nbMappings * sizeof(MoEntry))
	        : !handle.seek(head.dstOffset) || !handle.read(dst.get(), head.nbMappings * sizeof(MoEntry))) {
		return;
	}
	for (size_t i = 0; i < head.nbMappings; ++i) {
		SwapLE(dst[i]);
	}

	// MO header
	if (src[0].length != 0) {
		return;
	}
	{
		auto headerValue = std::unique_ptr<char[]> { new char[dst[0].length + 1] };
		if (readWholeFile
		        ? !ReadEntry(data.get(), fileSize, dst[0], &headerValue[0])
		        : !ReadEntry(handle, dst[0], &headerValue[0])) {
			return;
		}
		ParseMetadata(&headerValue[0]);
	}

	translation.resize(PluralForms);
	for (unsigned i = 0; i < PluralForms; i++)
		translation[i] = {};

	// Read strings described by entries
	size_t keysSize = 0;
	size_t valuesSize = 0;
	for (uint32_t i = 1; i < head.nbMappings; i++) {
		keysSize += src[i].length + 1;
		valuesSize += dst[i].length + 1;
	}
	translationKeys = std::unique_ptr<char[]> { new char[keysSize] };
	translationValues = std::unique_ptr<char[]> { new char[valuesSize] };

	char *keyPtr = &translationKeys[0];
	char *valuePtr = &translationValues[0];
	translation[0].reserve(head.nbMappings - 1);
	for (uint32_t i = 1; i < head.nbMappings; i++) {
		if (readWholeFile
		        ? ReadEntry(data.get(), fileSize, src[i], keyPtr) && ReadEntry(data.get(), fileSize, dst[i], valuePtr)
		        : ReadEntry(handle, src[i], keyPtr) && ReadEntry(handle, dst[i], valuePtr)) {
			// Plural keys also have a plural form but it does not participate in lookup.
			// Plural values are \0-terminated.
			std::string_view value { valuePtr, dst[i].length + 1 };
			for (size_t j = 0; j < PluralForms && !value.empty(); j++) {
				const size_t formValueEnd = value.find('\0');
				translation[j].emplace(keyPtr, EncodeTranslationRef(static_cast<uint32_t>(value.data() - &translationValues[0]), static_cast<uint32_t>(formValueEnd)));
				value.remove_prefix(formValueEnd + 1);
			}

			keyPtr += src[i].length + 1;
			valuePtr += dst[i].length + 1;
		}
	}

	LogVerbose(StrCat("Loaded translations from ", translationsPath, " in ", SDL_GetTicks() - loadTranslationsStart, "ms"));
}
