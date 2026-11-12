# Belial 64 – Diablo para Nintendo 64

Port gratuito de **Diablo** para **Nintendo 64**, por **Vanar** ([VanarTechlab](https://www.youtube.com/@VanarTechlab)).
Basado en [DevilutionX](https://github.com/diasurgical/devilutionX) y hecho con [libdragon](https://github.com/DragonMinded/libdragon).

**Versión actual: 0.9.5** · [Descargar](https://github.com/VanarTechlab/Belial-64/releases)

> **Esta es una versión modificada de DevilutionX.** Ver [MODIFICACIONES.md](MODIFICACIONES.md).
> Proyecto no oficial hecho por un fan, sin relación con Blizzard Entertainment ni con Nintendo.

## Descargar y jugar

La versión lista para jugar está en **[Releases](https://github.com/VanarTechlab/Belial-64/releases)**.
Es gratis y de libre distribución, y lo será siempre. **No incluye archivos de Blizzard**: necesitas tu propio
`DIABDAT.MPQ` de una copia original de Diablo (por ejemplo, la de GOG).

Necesitas:

- Nintendo 64 con **Expansion Pak** (obligatorio).
- Cartucho flash con tarjeta SD (**EverDrive 64 / ED64 Plus**).
- Un PC con Windows para el instalador.

El zip trae `Instalar Belial 64.exe`: eliges tu `DIABDAT.MPQ` y dónde instalar (la SD o una carpeta), y crea
la carpeta `devilutionx` (va en la raíz de la SD) y la ROM `belial64.z64`.

## Qué tiene

- Diablo completo con mando de N64, a 400x300 reducido a la pantalla.
- HUD a la derecha y minimapa siempre visible; texto de la caja de información y barra de experiencia propias.
- Teclado en pantalla para el nombre del personaje; borrar personajes.
- Música y sonidos desde la SD, copiados a la memoria del cartucho para que carguen rápido.
- Cinemáticas convertidas a MPEG-1 (logo, intro, finales) y menú para volver a verlas.
- Idioma español / English.
- Oro que se recoge solo, en un **monedero aparte** (no ocupa casillas del inventario), y cinturón que se rellena solo
  (en el cinturón solo van pociones).
- **Todas las misiones** disponibles en cada partida de un jugador.
- Wirt en el centro del pueblo y Farnham en un sitio nuevo.
- Dos misiones nuevas traídas de **Hellspawn** (el port de Vanar para Nintendo DSi): la guarida del Carnicero y las Luces del Bosque.
- Vibración con **Rumble Pak**.

## Novedades de la 0.9.5

- **Misión de la guarida del Carnicero**, con su escena oculta (el instalador la saca de tu `DIABDAT.MPQ`: es un vídeo
  que el juego original guarda pero no usa) y frases de los héroes que nunca se usaron ("What was that?").
- **Misión de las Luces del Bosque**: entrada al oeste del pueblo (nivel 18), súcubos y Andariel ("Time to die").
  Al terminarla, el bosque queda vacío y en paz. Dos árboles nuevos junto a la entrada.
- **"!"** encima de los aldeanos que tienen una misión para ti.
- **Gillian guarda y devuelve tu oro** (Guardar oro / Sacar oro en su menú).
- Montones de oro de hasta **60000** en el monedero: ya no sale "inventario lleno" con casillas libres.
- **Vibración con Rumble Pak**: golpes según el daño, muerte y latido del corazón desde el 50 % de vida (más fuerte
  cuanto menos vida). No vibra al guardar.
- **Pueblo más ligero**: los aldeanos, la vaca y las ventanas del alijo y de la tienda se leen de la memoria del
  cartucho; más memoria libre y menos bajones de fps. La búsqueda de objetivo cuerpo a cuerpo cuesta un tercio.
- **Más estable**: arreglados los cuelgues (pantalla azul o negra) tras volver varias veces al menú (la lista de héroes
  leía el guardado entero, 213 KiB, solo para mirar 4 bytes), los números denormales de la FPU que paraban la consola y
  los límites del mezclador de sonido.
- Probado de principio a fin: se puede terminar el juego, con la pelea contra Diablo fluida y la cinemática final.

## Novedades de la 0.8.5

- **Más estable:** memoria reservada al arrancar para la caché de gráficos, la pantalla de carga, el panel y el cursor.
  Se acabaron los cuelgues al guardar, al cambiar de piso, al entrar en un portal o al crear un héroe tras jugar.
- **Más fluido:** las cuevas van más rápidas (el ciclo de colores de la lava ya no rehace una tabla de 128 KiB en cada
  cuadro); con una tienda, un diálogo o el menú de START abiertos el mundo de detrás se congela y el menú va fluido.
- **Sonido:** sin música en las pantallas de carga (se entrecortaba), música del menú sin cortes al cambiar de menú,
  4 efectos a la vez y sin repetir el mismo efecto casi a la vez.
- Sin registro de diagnóstico en segundo plano: solo si hay un error grave se guardan las últimas líneas en
  `devilutionx/n64_log.txt` de la SD.

## Controles

| Botón | Acción |
|---|---|
| Palanca | Mover al personaje |
| A | Aceptar / atacar (ataca aunque no haya enemigo) |
| B | Lanzar el hechizo elegido (con un panel abierto: cerrarlo) |
| Z / R | Poción de vida / poción de maná |
| L | Lista rápida de hechizos |
| Cruceta | Arriba mapa, abajo hechizos, izquierda personaje, derecha inventario (en un panel: moverse por él) |
| C arriba / C izquierda | Misiones / acción secundaria (cofres, puertas, recoger) |
| START | Menú de la partida |
| L + R + C arriba / C abajo | Panel más grande / más pequeño |

## Código

Rama `main` = DevilutionX + los cambios de Belial 64. Los cambios en el motor están marcados con `#ifdef __N64__`.

| Ruta | Qué es |
|---|---|
| `n64/sdl_n64.c` | Capa SDL 1.2 propia para la N64: vídeo con el RDP, mando, SD del cartucho (DMA), memoria del cartucho como caché, hilos, audio, música, cinemáticas |
| `n64/sdl/include/` | Cabeceras de SDL 1.2 (LGPL-2.1) para compilar contra la capa propia |
| `CMake/platforms/n64.cmake` | Opciones de DevilutionX para la N64 |
| `n64/n64.toolchain.cmake` | Toolchain de CMake (libdragon) |
| `n64/compilar_sdl.sh`, `n64/configurar.sh`, `n64/compilar.sh` | Scripts de compilación |
| `n64/traduccion/` | Traducción al español (`es_n64.po` = la de DevilutionX + textos de Belial 64) y `msgfmt.py` |
| `n64/instalador/` | Instalador para Windows (Go): desempaqueta el MPQ y convierte gráficos, música, cinemáticas y la escena oculta del Carnicero |

## Compilar (Linux / WSL)

El enlazado envuelve varias funciones de la libc (`-Wl,--wrap=malloc,realloc,free,memalign,fread,fopen,...`, ver
`n64/n64.toolchain.cmake`): si rehaces `build-n64` a mano, comprueba que esas opciones siguen en `CMAKE_EXE_LINKER_FLAGS`.
Para una versión de pruebas con el registro de diagnóstico completo, compila `n64/sdl_n64.c` con `-DN64_RELEASE=0`.

1. Instala el toolchain de libdragon en `/opt/libdragon` (o pon la ruta en `N64_INST`) y libdragon rama
   **preview** (probado con el commit `39d0d60`): `make install tools-install`.
2. `n64/compilar_sdl.sh`
3. `n64/configurar.sh` (solo la primera vez; crea `build-n64`).
4. `n64/compilar.sh` → `n64/salida/belial64.z64` y `n64/salida/es.gmo`.

Instalador (con Go 1.22 o más nuevo, desde `n64/instalador`):

    GOOS=windows GOARCH=amd64 go build -tags gui -ldflags "-s -w -H windowsgui" -o "Instalar Belial 64.exe" .
    GOOS=windows GOARCH=amd64 go build -ldflags "-s -w" -o preparar_sd.exe .

El zip del Release lleva además `ffmpeg.exe` y `unpack_and_minify_mpq.exe` (de
[devilutionx-mpq-tools](https://github.com/diasurgical/devilutionx-mpq-tools)) en `herramientas\`, y los assets
de DevilutionX en `datos\devilutionx\assets`.

## Licencias

- DevilutionX: **Sustainable Use License 1.0** ([LICENSE.md](LICENSE.md)): uso y reparto **gratis y sin fines comerciales**. Belial 64 mantiene esa licencia.
- libdragon: dominio público (Unlicense). Cabeceras de SDL 1.2: LGPL-2.1. Lua: MIT.
- Diablo y sus datos son de Blizzard Entertainment y **no** están en este repositorio.

## Créditos

- **Vanar**: port a Nintendo 64 (y las misiones nuevas, de su port Hellspawn para DSi).
- **DevilutionX Team**: el motor.
- **libdragon** (Giovanni Bajo, Rasky y colaboradores): el SDK de N64.
- **Blizzard North**: creadores de Diablo.

Las donaciones son voluntarias y no desbloquean nada: <https://vanar-techlab-shop.fourthwall.com>

---

*English:* Free, non-commercial port of Diablo (DevilutionX) to Nintendo 64 (Expansion Pak + EverDrive 64 required).
Bring your own `DIABDAT.MPQ`. v0.9.5 adds the Butcher's lair and Forest Lights quests, quest markers,
gold storage, Rumble Pak support and stability fixes. This is a modified version of DevilutionX; see `MODIFICACIONES.md`.
Unofficial fan project, not affiliated with or endorsed by Blizzard Entertainment or Nintendo.
