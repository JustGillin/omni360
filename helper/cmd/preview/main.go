// preview writes the helper's icon as PNGs, to look at or to build the
// .exe's own icon from: go run ./cmd/preview <folder>
package main

import (
	"fmt"
	"os"
	"path/filepath"

	"omni360helper/icon"
)

func main() {
	dir := "."
	if len(os.Args) > 1 {
		dir = os.Args[1]
	}
	os.MkdirAll(dir, 0o755)
	for _, s := range []int{16, 20, 24, 32, 48, 64, 128, 256} {
		os.WriteFile(filepath.Join(dir, fmt.Sprintf("icon-%d.png", s)), icon.PNG(s, 0), 0o644)
	}
	for f := 0; f < 6; f++ {
		os.WriteFile(filepath.Join(dir, fmt.Sprintf("spin-%d.png", f)), icon.PNG(128, float64(f)*30), 0o644)
	}
}
