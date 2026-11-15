// Package icon draws Omni360 Helper's icon - a green disc on a white rounded
// square, after omni360_tray_icon.svg - at any size, with the disc turned by
// any angle. Drawn rather than scaled from one bitmap, so every size the tray
// asks for is sharp, and the disc can spin while something downloads.
package icon

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"math"
)

// The SVG's shapes as fractions of the white square's side (worked out from
// its transforms: the disc sits exactly at the square's centre).
const (
	cornerRadius = 0.3588
	discRadius   = 0.3757
	hubRadius    = 0.1274
	hubStroke    = 0.0213
	coreRadius   = 0.0703
	coreStroke   = 0.0128
	wedgeFrom    = 119.7 // degrees, y pointing down, before turning
	wedgeTo      = 154.8
)

var (
	white = color.RGBA{0xFF, 0xFF, 0xFF, 0xFF}
	green = color.RGBA{31, 160, 31, 0xFF}
	light = color.RGBA{143, 207, 143, 0xFF} // the SVG's half-white wedge over the green
	black = color.RGBA{0, 0, 0, 0xFF}
	core  = color.RGBA{26, 26, 26, 0xFF}
)

// grey turns a colour into a lighter grey of the same brightness, for the
// icon when no Xbox is connected.
func grey(c color.RGBA) color.RGBA {
	l := uint8((uint16(c.R)*30 + uint16(c.G)*59 + uint16(c.B)*11) / 100)
	l = l/2 + 96
	return color.RGBA{l, l, l, c.A}
}

// shade is the colour at (x, y) in the unit square, and whether anything is
// there at all (outside the rounded corners there isn't).
func shade(x, y, turn float64, dim bool) (color.RGBA, bool) {
	c, ok := shadeColour(x, y, turn)
	if ok && dim {
		c = grey(c)
	}
	return c, ok
}

func shadeColour(x, y, turn float64) (color.RGBA, bool) {
	// The rounded square.
	cx := math.Max(cornerRadius-x, math.Max(0, x-(1-cornerRadius)))
	cy := math.Max(cornerRadius-y, math.Max(0, y-(1-cornerRadius)))
	if x < cornerRadius || x > 1-cornerRadius {
		if y < cornerRadius || y > 1-cornerRadius {
			if cx*cx+cy*cy > cornerRadius*cornerRadius {
				return color.RGBA{}, false
			}
		}
	}
	if x < 0 || x > 1 || y < 0 || y > 1 {
		return color.RGBA{}, false
	}

	dx, dy := x-0.5, y-0.5
	d := math.Hypot(dx, dy)
	switch {
	case d < coreRadius-coreStroke/2:
		return core, true
	case d < coreRadius+coreStroke/2:
		return black, true
	case d < hubRadius-hubStroke/2:
		return white, true
	case d < hubRadius+hubStroke/2:
		return black, true
	case d < discRadius:
		a := math.Mod(math.Atan2(dy, dx)*180/math.Pi-turn+720, 180) // the two wedges are 180 degrees apart
		if a >= math.Mod(wedgeFrom, 180) && a <= math.Mod(wedgeTo, 180) {
			return light, true
		}
		return green, true
	}
	return white, true
}

// Render draws the icon size pixels square, the disc turned by turn degrees.
// Each pixel is the average of a 4x4 grid of samples, so edges are smooth.
func Render(size int, turn float64) *image.RGBA { return RenderStyle(size, turn, false) }

// RenderStyle is Render, greyed when dim.
func RenderStyle(size int, turn float64, dim bool) *image.RGBA {
	img := image.NewRGBA(image.Rect(0, 0, size, size))
	const n = 4
	for py := 0; py < size; py++ {
		for px := 0; px < size; px++ {
			var r, g, b, a float64
			for sy := 0; sy < n; sy++ {
				for sx := 0; sx < n; sx++ {
					x := (float64(px) + (float64(sx)+0.5)/n) / float64(size)
					y := (float64(py) + (float64(sy)+0.5)/n) / float64(size)
					if c, ok := shade(x, y, turn, dim); ok {
						r += float64(c.R)
						g += float64(c.G)
						b += float64(c.B)
						a += 255
					}
				}
			}
			if a == 0 {
				continue
			}
			// Premultiplied, as image.RGBA wants.
			k := float64(n * n)
			img.SetRGBA(px, py, color.RGBA{uint8(r / k), uint8(g / k), uint8(b / k), uint8(a / k)})
		}
	}
	return img
}

// PNG is Render as PNG bytes.
func PNG(size int, turn float64) []byte { return PNGStyle(size, turn, false) }

// PNGStyle is PNG, greyed when dim.
func PNGStyle(size int, turn float64, dim bool) []byte {
	var buf bytes.Buffer
	png.Encode(&buf, RenderStyle(size, turn, dim))
	return buf.Bytes()
}

// ICO holds the icon at every size Windows' tray and Explorer ask for, so
// Windows picks the one that fits instead of scaling one.
func ICO(turn float64) []byte { return ICOStyle(turn, false) }

// ICOStyle is ICO, greyed when dim.
func ICOStyle(turn float64, dim bool) []byte {
	sizes := []int{16, 20, 24, 32, 40, 48, 64, 256}
	images := make([][]byte, len(sizes))
	for i, s := range sizes {
		images[i] = PNGStyle(s, turn, dim)
	}

	var out bytes.Buffer
	binary.Write(&out, binary.LittleEndian, []uint16{0, 1, uint16(len(sizes))})
	offset := uint32(6 + 16*len(sizes))
	for i, s := range sizes {
		dim := byte(s)
		if s >= 256 {
			dim = 0 // 0 means 256 in an ICO directory
		}
		out.Write([]byte{dim, dim, 0, 0})
		binary.Write(&out, binary.LittleEndian, []uint16{1, 32})
		binary.Write(&out, binary.LittleEndian, []uint32{uint32(len(images[i])), offset})
		offset += uint32(len(images[i]))
	}
	for _, img := range images {
		out.Write(img)
	}
	return out.Bytes()
}
