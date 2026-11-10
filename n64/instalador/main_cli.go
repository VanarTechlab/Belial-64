//go:build !gui

package main

// preparar_sd: la misma instalacion que la ventana, pero desde la consola.
// Uso: preparar_sd DIABDAT.MPQ CARPETA_DESTINO [--sin-cinematicas]
//      (con "-" en vez del MPQ usa el diabdat que ya este en CARPETA_DESTINO\devilutionx)

import (
	"fmt"
	"os"
	"path/filepath"
)

func main() {
	args := []string{}
	cine := true
	for _, a := range os.Args[1:] {
		if a == "--sin-cinematicas" {
			cine = false
		} else {
			args = append(args, a)
		}
	}
	if len(args) != 2 {
		fmt.Println("Uso: preparar_sd DIABDAT.MPQ CARPETA_DESTINO [--sin-cinematicas]")
		os.Exit(2)
	}
	exe, _ := os.Executable()
	o := &Options{Base: filepath.Dir(exe), MPQ: args[0], Dest: args[1], Cinematicas: cine,
		Log: func(s string) { fmt.Println(s) }}
	if b := os.Getenv("BELIAL64_BASE"); b != "" {
		o.Base = b
	}
	if o.MPQ == "-" {
		o.MPQ = ""
	}
	if err := Run(o); err != nil {
		fmt.Println("\n*** ERROR:", err)
		os.Exit(1)
	}
}
