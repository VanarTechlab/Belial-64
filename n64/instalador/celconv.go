package main

// Conversion de los CEL de nivel al formato que usa Belial 64 (port de DevilutionX a N64).
// Es la misma conversion que hace DevilutionX al cargar un nivel (ReencodeDungeonCels en
// Source/levels/reencode_dun_cels.cpp y SetDungeonMicros en Source/levels/dun_tile_data.cpp),
// hecha aqui en el PC porque en la N64 el CEL no cabe en la memoria.

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"sort"
)

const (
	dunW = 32
	dunH = 32

	tSquare      = 0
	tTransSquare = 1
	tLeftTri     = 2
	tRightTri    = 3
	tLeftTrap    = 4
	tRightTrap   = 5

	pSolid        = 1 << 0
	pBlockLight   = 1 << 1
	pBlockMissile = 1 << 2

	maxTiles = 1379
)

type frameInfo struct {
	frame uint16
	micro uint8
	typ   uint8
	props uint8
}

func (f frameInfo) isFloor() bool {
	return f.props&(pSolid|pBlockMissile) == 0 && (f.micro == 0 || f.micro == 1)
}

type writer struct {
	b []byte
}

func (w *writer) put(p []byte) { w.b = append(w.b, p...) }

func leftTriLower(w *writer, src []byte, s int) int {
	width := 0
	for i := 0; i < 8; i++ {
		s += 2
		width += 2
		w.put(src[s : s+width])
		s += width
		width += 2
		w.put(src[s : s+width])
		s += width
	}
	return s
}

func leftTri(w *writer, src []byte, s int) {
	s = leftTriLower(w, src, s)
	width := dunW
	for i := 0; i < 7; i++ {
		s += 2
		width -= 2
		w.put(src[s : s+width])
		s += width
		width -= 2
		w.put(src[s : s+width])
		s += width
	}
	s += 2
	width -= 2
	w.put(src[s : s+width])
}

func rightTriLower(w *writer, src []byte, s int) int {
	width := 0
	for i := 0; i < 8; i++ {
		width += 2
		w.put(src[s : s+width])
		s += width + 2
		width += 2
		w.put(src[s : s+width])
		s += width
	}
	return s
}

func rightTri(w *writer, src []byte, s int) {
	s = rightTriLower(w, src, s)
	width := dunW
	for i := 0; i < 7; i++ {
		width -= 2
		w.put(src[s : s+width])
		s += width + 2
		width -= 2
		w.put(src[s : s+width])
		s += width
	}
	width -= 2
	w.put(src[s : s+width])
}

func leftTrap(w *writer, src []byte, s int) {
	s = leftTriLower(w, src, s)
	w.put(src[s : s+dunW*16])
}

func rightTrap(w *writer, src []byte, s int) {
	s = rightTriLower(w, src, s)
	w.put(src[s : s+dunW*16])
}

// floorWithFoliage: el cuadro transparente de un suelo se separa en un triangulo (suelo) y un
// cuadro transparente de 16 lineas (follaje), como ReencodeFloorWithFoliage.
func floorWithFoliage(w *writer, src []byte, s int, left bool) {
	var surf [dunW * dunH]byte
	// RenderTransparentSquare: empieza en la ultima linea y sube
	d := dunW * (dunH - 1)
	for i := 0; i < dunH; i++ {
		x := 0
		for x < dunW {
			v := int(int8(src[s]))
			s++
			if v > 0 {
				copy(surf[d+x:d+x+v], src[s:s+v])
				s += v
			} else {
				v = -v
			}
			x += v
		}
		d -= dunW
	}
	last := dunW * (dunH - 1)
	// triangulo
	p := last
	if left {
		for wd, y := 2, 31; y >= 16; y, wd, p = y-1, wd+2, p-dunW {
			w.put(surf[p+dunW-wd : p+dunW])
			for k := p + dunW - wd; k < p+dunW; k++ {
				surf[k] = 0
			}
		}
		for wd, y := 30, 15; y > 0; y, wd, p = y-1, wd-2, p-dunW {
			w.put(surf[p+dunW-wd : p+dunW])
			for k := p + dunW - wd; k < p+dunW; k++ {
				surf[k] = 0
			}
		}
	} else {
		for wd, y := 2, 31; y >= 16; y, wd, p = y-1, wd+2, p-dunW {
			w.put(surf[p : p+wd])
			for k := p; k < p+wd; k++ {
				surf[k] = 0
			}
		}
		for wd, y := 30, 15; y > 0; y, wd, p = y-1, wd-2, p-dunW {
			w.put(surf[p : p+wd])
			for k := p; k < p+wd; k++ {
				surf[k] = 0
			}
		}
	}
	// ExtractFoliageTransparentSquare (src -= DunFrameHeight*16; por linea src -= 2*W tras avanzar W)
	q := last - dunH*16
	for y := 16; y > 0; y-- {
		trans, solid := 0, 0
		for x := 0; x < dunW; x++ {
			if surf[q] != 0 {
				if trans != 0 {
					w.b = append(w.b, byte(-int8(trans)))
					trans = 0
				}
				solid++
			} else {
				if solid != 0 {
					w.b = append(w.b, byte(solid))
					w.put(surf[q-solid : q])
					solid = 0
				}
				trans++
			}
			q++
		}
		if trans != 0 {
			w.b = append(w.b, byte(-int8(trans)))
		} else if solid != 0 {
			w.b = append(w.b, byte(solid))
			w.put(surf[q-solid : q])
		}
		q -= 2 * dunW
	}
}

// reencode devuelve el CEL convertido (igual que el volcado N64_DUMP_CELS del PC)
func reencode(cel []byte, frames []frameInfo) ([]byte, error) {
	off := func(i int) int { return int(binary.LittleEndian.Uint32(cel[i*4:])) }
	hdr := (2 + len(frames)) * 4
	w := &writer{b: make([]byte, hdr, len(cel)+hdr)}
	binary.LittleEndian.PutUint32(w.b[0:], uint32(len(frames)))
	nf := off(0)
	for i, f := range frames {
		if int(f.frame) >= nf+1 {
			return nil, fmt.Errorf("trozo %d fuera del CEL", f.frame)
		}
		binary.LittleEndian.PutUint32(w.b[4+i*4:], uint32(len(w.b)))
		s := off(int(f.frame))
		switch f.typ {
		case tTransSquare:
			if f.isFloor() {
				floorWithFoliage(w, cel, s, f.micro == 0)
			} else {
				w.put(cel[s:off(int(f.frame)+1)])
			}
		case tSquare:
			w.put(cel[s : s+dunW*dunH])
		case tLeftTri:
			leftTri(w, cel, s)
		case tRightTri:
			rightTri(w, cel, s)
		case tLeftTrap:
			leftTrap(w, cel, s)
		case tRightTrap:
			rightTrap(w, cel, s)
		default:
			return nil, fmt.Errorf("tipo de trozo %d desconocido", f.typ)
		}
	}
	binary.LittleEndian.PutUint32(w.b[4+len(frames)*4:], uint32(len(w.b)))
	return w.b, nil
}

type levelDef struct {
	dir, name string // levels\<dir>\<name>.cel
	blocks    int
	patch     func(sol []byte)
}

func patchL1(sol []byte) {
	bl := byte(pBlockLight | pBlockMissile)
	for _, i := range []int{9, 15, 16, 20, 21, 51, 56, 58, 61, 63, 65, 72, 208, 247, 253, 257, 323} {
		sol[i] |= bl
	}
	sol[27] |= pBlockMissile
	sol[28] |= pBlockMissile
	sol[403] |= pBlockLight
	sol[24] |= pBlockLight
	sol[450] |= bl
}

func patchL3(sol []byte) {
	sol[170] |= pBlockMissile
	sol[481] |= pSolid
	sol[487] |= pSolid
}

func patchL4(sol []byte) { sol[210] = 0 }

var levels = []levelDef{
	{"towndata", "town", 16, nil},
	{"l1data", "l1", 10, patchL1},
	{"l2data", "l2", 10, nil},
	{"l3data", "l3", 10, patchL3},
	{"l4data", "l4", 16, patchL4},
}

func readCI(dir string, parts ...string) ([]byte, error) {
	// el diabdat desempaquetado puede tener mayusculas o minusculas
	p := dir
	for _, part := range parts {
		next := filepath.Join(p, part)
		if _, err := os.Stat(next); err != nil {
			ents, e2 := os.ReadDir(p)
			if e2 != nil {
				return nil, err
			}
			found := false
			for _, e := range ents {
				if equalFold(e.Name(), part) {
					next = filepath.Join(p, e.Name())
					found = true
					break
				}
			}
			if !found {
				return nil, err
			}
		}
		p = next
	}
	return os.ReadFile(p)
}

func equalFold(a, b string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := 0; i < len(a); i++ {
		x, y := a[i], b[i]
		if 'A' <= x && x <= 'Z' {
			x += 32
		}
		if 'A' <= y && y <= 'Z' {
			y += 32
		}
		if x != y {
			return false
		}
	}
	return true
}

func convertLevel(diabdat string, lv levelDef) ([]byte, int, error) {
	cel, err := readCI(diabdat, "levels", lv.dir, lv.name+".cel")
	if err != nil {
		return nil, 0, err
	}
	minb, err := readCI(diabdat, "levels", lv.dir, lv.name+".min")
	if err != nil {
		return nil, 0, err
	}
	solf, err := readCI(diabdat, "levels", lv.dir, lv.name+".sol")
	if err != nil {
		return nil, 0, err
	}
	sol := make([]byte, maxTiles+1)
	copy(sol, solf)
	if lv.patch != nil {
		lv.patch(sol)
	}
	tileCount := len(minb) / 2
	pieces := tileCount / lv.blocks
	if pieces > maxTiles {
		pieces = maxTiles
	}
	seen := map[uint16]frameInfo{}
	for id := 0; id < pieces; id++ {
		base := lv.blocks * id
		for block := 0; block < lv.blocks; block++ {
			idx := base + lv.blocks - 2 + (block & 1) - (block & 0xE)
			v := binary.LittleEndian.Uint16(minb[idx*2:])
			if v == 0 {
				continue
			}
			fr := v & 0xFFF
			if _, ok := seen[fr]; !ok {
				seen[fr] = frameInfo{fr, uint8(block), uint8((v & 0x7000) >> 12), sol[id]}
			}
		}
	}
	list := make([]frameInfo, 0, len(seen))
	for _, f := range seen {
		list = append(list, f)
	}
	sort.Slice(list, func(i, j int) bool { return list[i].frame < list[j].frame })
	out, err := reencode(cel, list)
	return out, len(list), err
}

// sumFile: FNV-1a de 32 bits de cada bloque de 32 KiB (little endian), como hacer_sum.py
func sumFile(d []byte) []byte {
	out := make([]byte, 0, (len(d)/32768+1)*4)
	for i := 0; i < len(d); i += 32768 {
		h := uint32(0x811C9DC5)
		end := i + 32768
		if end > len(d) {
			end = len(d)
		}
		for _, b := range d[i:end] {
			h = (h ^ uint32(b)) * 0x01000193
		}
		out = binary.LittleEndian.AppendUint32(out, h)
	}
	return out
}

// convertAllCels escribe los .cel.n64 y sus .sum en dstN64 (carpeta devilutionx\n64)
func convertAllCels(diabdat, dstN64 string, logf func(string, ...any)) error {
	for _, lv := range levels {
		data, n, err := convertLevel(diabdat, lv)
		if err != nil {
			return fmt.Errorf("nivel %s: %v", lv.name, err)
		}
		names := []string{"levels_" + lv.dir + "_" + lv.name + ".cel.n64"}
		if lv.name == "town" {
			names = append(names, "nlevels_"+lv.dir+"_"+lv.name+".cel.n64")
		}
		for _, nm := range names {
			p := filepath.Join(dstN64, nm)
			if err := os.WriteFile(p, data, 0o644); err != nil {
				return err
			}
			if err := os.WriteFile(p+".sum", sumFile(data), 0o644); err != nil {
				return err
			}
		}
		logf("  %s: %d trozos, %d KiB", lv.name, n, len(data)/1024)
	}
	return nil
}
