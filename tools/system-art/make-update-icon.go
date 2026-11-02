// Generates the Update Available system art from MinUI's existing faux-system art.
// It recovers the shared background by taking the darkest reference pixel, then draws a matching outlined arrow.
package main

import (
	"fmt"
	"image"
	"image/color"
	"image/png"
	"math"
	"os"
)

type point struct{ x, y float64 }

func load(path string) image.Image {
	f, err := os.Open(path)
	if err != nil {
		panic(err)
	}
	defer f.Close()
	im, err := png.Decode(f)
	if err != nil {
		panic(err)
	}
	return im
}

func distanceToSegment(p, a, b point) float64 {
	dx, dy := b.x-a.x, b.y-a.y
	t := ((p.x-a.x)*dx + (p.y-a.y)*dy) / (dx*dx + dy*dy)
	if t < 0 {
		t = 0
	} else if t > 1 {
		t = 1
	}
	x, y := a.x+t*dx, a.y+t*dy
	return math.Hypot(p.x-x, p.y-y)
}

func main() {
	if len(os.Args) != 5 {
		fmt.Fprintln(os.Stderr, "usage: make-update-icon FAVORITES RECENTLY_PLAYED TOOLS OUTPUT")
		os.Exit(2)
	}
	refs := []image.Image{load(os.Args[1]), load(os.Args[2]), load(os.Args[3])}
	bounds := refs[0].Bounds()
	for _, ref := range refs[1:] {
		if ref.Bounds() != bounds {
			panic("reference dimensions differ")
		}
	}
	out := image.NewRGBA(bounds)

	// The icons differ but the background is identical. Their light pixels disappear by keeping the darkest RGB
	// value at each position, preserving the original diagonal edge and its antialiasing exactly.
	for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
		for x := bounds.Min.X; x < bounds.Max.X; x++ {
			r, g, b := uint32(0xffff), uint32(0xffff), uint32(0xffff)
			for _, ref := range refs {
				rr, gg, bb, _ := ref.At(x, y).RGBA()
				if rr < r {
					r = rr
				}
				if gg < g {
					g = gg
				}
				if bb < b {
					b = bb
				}
			}
			out.SetRGBA(x, y, color.RGBA{uint8(r >> 8), uint8(g >> 8), uint8(b >> 8), 255})
		}
	}

	// A closed, hollow up arrow. Distance-based supersampling gives the softly rounded joins and 8px line weight
	// used by Favorites, Recently Played and Tools.
	arrow := []point{{120, 169}, {183, 235}, {151, 235}, {151, 302}, {89, 302}, {89, 235}, {57, 235}}
	const samples = 6
	const halfStroke = 4.25
	ink := color.RGBA{224, 224, 224, 255}
	for y := 158; y <= 312; y++ {
		for x := 47; x <= 193; x++ {
			covered := 0
			for sy := 0; sy < samples; sy++ {
				for sx := 0; sx < samples; sx++ {
					p := point{float64(x) + (float64(sx)+0.5)/samples, float64(y) + (float64(sy)+0.5)/samples}
					hit := false
					for i, a := range arrow {
						b := arrow[(i+1)%len(arrow)]
						if distanceToSegment(p, a, b) <= halfStroke {
							hit = true
							break
						}
					}
					if hit {
						covered++
					}
				}
			}
			if covered == 0 {
				continue
			}
			base := out.RGBAAt(x, y)
			a := float64(covered) / float64(samples*samples)
			blend := func(bg, fg uint8) uint8 { return uint8(math.Round(float64(bg)*(1-a) + float64(fg)*a)) }
			out.SetRGBA(x, y, color.RGBA{blend(base.R, ink.R), blend(base.G, ink.G), blend(base.B, ink.B), 255})
		}
	}

	f, err := os.Create(os.Args[4])
	if err != nil {
		panic(err)
	}
	if err := png.Encode(f, out); err != nil {
		panic(err)
	}
	if err := f.Close(); err != nil {
		panic(err)
	}
}
