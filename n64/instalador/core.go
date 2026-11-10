package main

// Preparacion de Belial 64: a partir del DIABDAT.MPQ del jugador crea la carpeta devilutionx
// (datos del juego desempaquetados + conversiones para la N64) y copia la ROM belial64.z64.
// No incluye ningun archivo de Blizzard: todo sale del MPQ del propio jugador.

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

type Options struct {
	Base        string // carpeta del instalador (con herramientas\ y datos\)
	MPQ         string // DIABDAT.MPQ del jugador ("" = usar el diabdat que ya hay en el destino)
	Dest        string // carpeta de destino (raiz de la SD o cualquier carpeta)
	Cinematicas bool
	Log         func(string)
	Progress    func(int) // 0..100
}

func (o *Options) logf(f string, a ...any) { o.Log(fmt.Sprintf(f, a...)) }

func toolPath(base, name string) string {
	if p := filepath.Join(base, "herramientas", name+exeSuffix); fileExists(p) {
		return p
	}
	if p, err := exec.LookPath(name); err == nil { // en Linux (pruebas) el del sistema
		return p
	}
	return filepath.Join(base, "herramientas", name+exeSuffix)
}

func fileExists(p string) bool { st, err := os.Stat(p); return err == nil && !st.IsDir() }
func dirExists(p string) bool  { st, err := os.Stat(p); return err == nil && st.IsDir() }

// findCI busca una entrada de la carpeta sin importar mayusculas
func findCI(dir, name string) string {
	if p := filepath.Join(dir, name); fileExists(p) || dirExists(p) {
		return p
	}
	ents, _ := os.ReadDir(dir)
	for _, e := range ents {
		if equalFold(e.Name(), name) {
			return filepath.Join(dir, e.Name())
		}
	}
	return ""
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

func copyTree(src, dst string) (int, error) {
	n := 0
	err := filepath.Walk(src, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(src, p)
		t := filepath.Join(dst, rel)
		if info.IsDir() {
			return os.MkdirAll(t, 0o755)
		}
		n++
		return copyFile(p, t)
	})
	return n, err
}

func filesWithExt(dir, ext string) []string {
	var out []string
	ents, _ := os.ReadDir(dir)
	for _, e := range ents {
		if !e.IsDir() && strings.EqualFold(filepath.Ext(e.Name()), ext) {
			out = append(out, e.Name())
		}
	}
	sort.Strings(out)
	return out
}

func runTool(o *Options, exe string, args ...string) error {
	cmd := exec.Command(exe, args...)
	hideWindow(cmd)
	out, err := cmd.CombinedOutput()
	if err != nil {
		s := strings.TrimSpace(string(out))
		if len(s) > 600 {
			s = s[len(s)-600:]
		}
		if s != "" {
			o.Log("    " + strings.ReplaceAll(s, "\n", "\n    "))
		}
		return err
	}
	return nil
}

// unpackMPQ desempaqueta el MPQ con unpack_and_minify_mpq (DevilutionX) en dx\diabdat
func unpackMPQ(o *Options, dx string) (string, error) {
	tool := toolPath(o.Base, "unpack_and_minify_mpq")
	if !fileExists(tool) {
		return "", errors.New("falta herramientas\\unpack_and_minify_mpq.exe (descomprime de nuevo el zip completo)")
	}
	tmp := filepath.Join(dx, "_desempaquetando")
	os.RemoveAll(tmp)
	if err := os.MkdirAll(tmp, 0o755); err != nil {
		return "", err
	}
	if err := runTool(o, tool, "--output-dir", tmp, o.MPQ); err != nil {
		o.logf("    (el desempaquetador termino con un aviso: %v; se comprueba el resultado)", err)
	}
	// el desempaquetador crea una carpeta con el nombre del MPQ (diabdat, DIABDAT...)
	var got string
	if findCI(tmp, "levels") != "" {
		got = tmp
	}
	ents, _ := os.ReadDir(tmp)
	for _, e := range ents {
		if got == "" && e.IsDir() && findCI(filepath.Join(tmp, e.Name()), "levels") != "" {
			got = filepath.Join(tmp, e.Name())
			break
		}
	}
	if got == "" {
		os.RemoveAll(tmp)
		return "", errors.New("no se pudo desempaquetar el MPQ (¿es el DIABDAT.MPQ de Diablo completo?)")
	}
	final := filepath.Join(dx, "diabdat")
	if dirExists(final) {
		o.Log("    borrando el diabdat anterior...")
		if err := os.RemoveAll(final); err != nil {
			return "", fmt.Errorf("no se pudo borrar el diabdat anterior: %v", err)
		}
	}
	if err := os.Rename(got, final); err != nil {
		return "", err
	}
	if got != tmp {
		os.RemoveAll(tmp)
	}
	return final, nil
}

func checkDiabdat(d string) error {
	if findCI(d, "levels") == "" || findCI(d, "monsters") == "" {
		return errors.New("el diabdat no tiene las carpetas levels y monsters")
	}
	if findCI(d, "music") == "" {
		return errors.New("el MPQ no tiene música: parece la demo (spawn.mpq). Hace falta el DIABDAT.MPQ de Diablo completo")
	}
	return nil
}

func convertMusic(o *Options, ffmpeg, diabdat, dst string) (int, error) {
	src := findCI(diabdat, "music")
	if err := os.MkdirAll(dst, 0o755); err != nil {
		return 0, err
	}
	n := 0
	for _, f := range filesWithExt(src, ".wav") {
		out := filepath.Join(dst, strings.ToLower(f))
		o.logf("    música %s", f)
		if err := runTool(o, ffmpeg, "-hide_banner", "-v", "error", "-y", "-i", filepath.Join(src, f),
			"-ac", "1", "-ar", "22050", "-c:a", "adpcm_ima_wav", out); err != nil {
			return n, fmt.Errorf("ffmpeg fallo con %s: %v", f, err)
		}
		n++
	}
	os.WriteFile(filepath.Join(dst, "LEEME.txt"), []byte(
		"Musica de tu DIABDAT.MPQ convertida a IMA ADPCM mono 22050 Hz para Belial 64.\r\n"+
			"Uso personal: no la compartas (es de Blizzard).\r\n"), 0o644)
	return n, nil
}

// las cinematicas que usa el juego (fprst3 no se usa)
var cineNames = []string{"logo", "diablo1", "diabend", "loopdend", "diabvic1", "diabvic2", "diabvic3"}

// mismos ajustes que videoconv64 de libdragon (-w 320 -r 30 -q 80) para videos de 320x156
const cineVF = "scale=320:144:flags=lanczos:out_color_matrix=bt601:out_range=tv,fps=30.000:eof_action=pass," +
	"format=yuv420p,hqdn3d=0.80:0.80:4:4,gradfun=6:16,unsharp=5:5:0.30:3:3:0.0"
const cineMatrix = "8,16,22,26,30,35,40,48,16,16,26,28,32,35,40,48,22,26,30,32,35,40,48,56,26,28,32,35,40,48,56,64," +
	"30,32,35,40,48,56,64,80,35,35,40,48,56,64,80,96,40,40,48,56,64,80,96,128,48,48,56,64,80,128,128,150"

func convertCine(o *Options, ffmpeg, diabdat, dst string) (int, error) {
	gendata := findCI(diabdat, "gendata")
	if err := os.MkdirAll(dst, 0o755); err != nil {
		return 0, err
	}
	tmpLog := filepath.Join(os.TempDir(), "belial64_pass")
	null := "/dev/null"
	if exeSuffix != "" {
		null = "NUL"
	}
	n := 0
	for i, name := range cineNames {
		src := findCI(gendata, name+".smk")
		if src == "" {
			o.logf("    (no esta %s.smk, se salta)", name)
			continue
		}
		o.logf("    cinemática %s (%d de %d)", name, i+1, len(cineNames))
		base := []string{"-hide_banner", "-v", "error", "-y", "-i", src, "-an", "-vf", cineVF,
			"-c:v", "mpeg1video", "-b:v", "800k", "-maxrate", "800k", "-bufsize", "640k", "-bf", "2",
			"-trellis", "2", "-mbd", "rd", "-cmp", "rd", "-subcmp", "rd", "-intra_matrix", cineMatrix}
		out := filepath.Join(dst, name+".m1v")
		p1 := append(append([]string{}, base...), "-pass", "1", "-passlogfile", tmpLog, "-f", "null", null)
		p2 := append(append([]string{}, base...), "-pass", "2", "-passlogfile", tmpLog, "-f", "mpeg1video", out)
		if err := runTool(o, ffmpeg, p1...); err != nil {
			return n, fmt.Errorf("ffmpeg fallo con %s (paso 1): %v", name, err)
		}
		if err := runTool(o, ffmpeg, p2...); err != nil {
			return n, fmt.Errorf("ffmpeg fallo con %s (paso 2): %v", name, err)
		}
		os.Remove(tmpLog + "-0.log")
		os.Remove(tmpLog + "-0.log.mbtree")
		// como videoconv64: proporcion de pixel 12:13 (320x156 -> 320x144) como user_data al principio
		if data, err := os.ReadFile(out); err == nil {
			hdr := append([]byte{0, 0, 1, 0xB2}, []byte("LD_SAR=12:13\n")...)
			if err := os.WriteFile(out, append(hdr, data...), 0o644); err != nil {
				return n, err
			}
		}
		// sonido (loopdend no tiene)
		wav := filepath.Join(dst, name+".wav")
		os.Remove(wav)
		_ = runTool(o, ffmpeg, "-hide_banner", "-v", "quiet", "-y", "-i", src, "-vn", "-ac", "1", "-ar", "22050",
			"-c:a", "adpcm_ima_wav", wav)
		if st, err := os.Stat(wav); err == nil && st.Size() < 100 {
			os.Remove(wav)
		}
		n++
		if o.Progress != nil {
			o.Progress(70 + (i+1)*25/len(cineNames))
		}
	}
	return n, nil
}

// Run hace toda la preparacion
func Run(o *Options) error {
	prog := func(p int) {
		if o.Progress != nil {
			o.Progress(p)
		}
	}
	if o.Dest == "" {
		return errors.New("elige la carpeta de destino")
	}
	dx := filepath.Join(o.Dest, "devilutionx")
	if err := os.MkdirAll(dx, 0o755); err != nil {
		return fmt.Errorf("no se puede escribir en %s: %v", o.Dest, err)
	}
	rom := filepath.Join(o.Base, "datos", "belial64.z64")
	assets := filepath.Join(o.Base, "datos", "devilutionx", "assets")
	if !fileExists(rom) || !dirExists(assets) {
		return errors.New("faltan archivos del instalador (datos\\belial64.z64 o datos\\devilutionx\\assets): descomprime de nuevo el zip completo")
	}
	ffmpeg := toolPath(o.Base, "ffmpeg")
	if !fileExists(ffmpeg) {
		return errors.New("falta herramientas\\ffmpeg.exe (descomprime de nuevo el zip completo)")
	}

	o.Log("[1/6] Desempaquetando tu DIABDAT.MPQ (unos minutos)...")
	prog(3)
	var diabdat string
	if o.MPQ != "" {
		d, err := unpackMPQ(o, dx)
		if err != nil {
			return err
		}
		diabdat = d
	} else {
		diabdat = findCI(dx, "diabdat")
		if diabdat == "" {
			return errors.New("elige tu DIABDAT.MPQ")
		}
		o.Log("    se usa el diabdat que ya estaba")
	}
	if err := checkDiabdat(diabdat); err != nil {
		return err
	}
	prog(30)

	o.Log("[2/6] Copiando los archivos de DevilutionX (assets y traduccion)...")
	n, err := copyTree(assets, filepath.Join(dx, "assets"))
	if err != nil {
		return fmt.Errorf("no se pudieron copiar los assets: %v", err)
	}
	o.logf("    %d archivos", n)
	prog(35)

	o.Log("[3/6] Preparando los gráficos de los niveles para la N64...")
	n64 := filepath.Join(dx, "n64")
	if err := os.MkdirAll(n64, 0o755); err != nil {
		return err
	}
	if err := convertAllCels(diabdat, n64, func(f string, a ...any) { o.logf(f, a...) }); err != nil {
		return err
	}
	prog(45)

	o.Log("[4/6] Convirtiendo la música (más pequeña, para que cargue rápido)...")
	nm, err := convertMusic(o, ffmpeg, diabdat, filepath.Join(n64, "music"))
	if err != nil {
		return err
	}
	o.logf("    %d pistas", nm)
	prog(70)

	if o.Cinematicas {
		o.Log("[5/6] Convirtiendo las cinemáticas a vídeo de N64 (tarda varios minutos)...")
		nc, err := convertCine(o, ffmpeg, diabdat, filepath.Join(n64, "cine"))
		if err != nil {
			return err
		}
		o.logf("    %d cinemáticas", nc)
	} else {
		o.Log("[5/6] Cinemáticas: no (se pueden añadir después volviendo a instalar)")
	}
	prog(95)

	o.Log("[6/6] Copiando la ROM belial64.z64...")
	if err := copyFile(rom, filepath.Join(o.Dest, "belial64.z64")); err != nil {
		return err
	}
	os.WriteFile(filepath.Join(o.Dest, "LEEME_Belial64.txt"), append([]byte{0xEF, 0xBB, 0xBF}, leemeDest...), 0o644)
	prog(100)
	o.Log("")
	o.Log("LISTO. En " + o.Dest + " tienes:")
	o.Log("   - la carpeta devilutionx  -> cópiala a la RAÍZ de la SD del cartucho (si no la elegiste ya)")
	o.Log("   - belial64.z64            -> cópiala a cualquier carpeta de la SD")
	o.Log("Necesitas el Expansion Pak. Abre belial64.z64 desde el menú del cartucho.")
	return nil
}

const leemeDest = "BELIAL 64 - Diablo para Nintendo 64 (por Vanar)\r\n" +
	"================================================\r\n\r\n" +
	"1. Copia la carpeta \"devilutionx\" a la RAÍZ de la tarjeta SD del cartucho flash\r\n" +
	"   (EverDrive 64 / ED64 Plus). Debe quedar como  SD:\\devilutionx\\\r\n" +
	"2. Copia \"belial64.z64\" a cualquier carpeta de la SD.\r\n" +
	"3. Pon el Expansion Pak en la consola (obligatorio) y abre belial64.z64.\r\n\r\n" +
	"La carpeta devilutionx contiene datos sacados de TU DIABDAT.MPQ: son para tu uso\r\n" +
	"personal. No los compartas ni los subas a internet.\r\n\r\n" +
	"Belial 64 es un proyecto no oficial hecho por un fan, gratuito y de libre distribución.\r\n" +
	"No está afiliado ni respaldado por Blizzard Entertainment ni por Nintendo.\r\n" +
	"Diablo es una marca de Blizzard Entertainment. Nintendo 64 es una marca de Nintendo.\r\n"
