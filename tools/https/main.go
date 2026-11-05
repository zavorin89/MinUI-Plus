// https is MinUI+'s small HTTPS-only downloader for devices whose BusyBox wget was built without TLS.
package main

import (
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"regexp"
	"strings"
	"time"
)

const defaultCA = "/mnt/SDCARD/.system/res/cacert.pem"

func fail(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "https: "+format+"\n", args...)
	os.Exit(1)
}

// GitLab reports neither a checksum nor a size for release files, so a release's notes carry them on lines of their own.
var (
	checksumLine = regexp.MustCompile("(?i)^[\\s*`]*SHA-256:[\\s*`]*([0-9a-f]{64})[\\s*`]*$")
	sizeLine     = regexp.MustCompile("(?i)^[\\s*`]*Size:[\\s*`]*([0-9]+)[\\s*`]*$")
)

// normalizeRelease turns a GitLab release into what update.sh reads: five lines (tag, asset, URL, SHA-256, size)
// followed by the notes, without the checksum and size lines.
func normalizeRelease(body io.Reader) (string, error) {
	var parsed struct {
		TagName     string `json:"tag_name"`
		Description string `json:"description"`
		Assets      struct {
			Links []struct {
				Name           string `json:"name"`
				URL            string `json:"url"`
				DirectAssetURL string `json:"direct_asset_url"`
			} `json:"links"`
		} `json:"assets"`
	}
	if err := json.NewDecoder(body).Decode(&parsed); err != nil {
		return "", fmt.Errorf("decode GitLab release: %v", err)
	}
	var digest, size string
	var notes []string
	for _, line := range strings.Split(strings.ReplaceAll(parsed.Description, "\r\n", "\n"), "\n") {
		if m := checksumLine.FindStringSubmatch(line); m != nil {
			digest = strings.ToLower(m[1])
		} else if m := sizeLine.FindStringSubmatch(line); m != nil {
			size = m[1]
		} else {
			notes = append(notes, line)
		}
	}
	for _, asset := range parsed.Assets.Links {
		if strings.HasPrefix(asset.Name, "MinUIPlus-") && strings.HasSuffix(asset.Name, ".zip") {
			url := asset.DirectAssetURL
			if url == "" {
				url = asset.URL
			}
			return fmt.Sprintf("%s\n%s\n%s\n%s\n%s\n%s\n", parsed.TagName, asset.Name, url, digest, size,
				strings.TrimSpace(strings.Join(notes, "\n"))), nil
		}
	}
	return "", fmt.Errorf("GitLab release has no MinUIPlus ZIP asset")
}

func main() {
	timeout := flag.Duration("t", 30*time.Second, "total request timeout")
	release := flag.Bool("release", false, "normalize a GitLab release for the updater")
	flag.Usage = func() { fmt.Fprintln(os.Stderr, "usage: https.elf [-t 30s] [-release] URL [OUTPUT]") }
	flag.Parse()
	if flag.NArg() < 1 || flag.NArg() > 2 {
		flag.Usage()
		os.Exit(2)
	}

	url := flag.Arg(0)
	if len(url) < 8 || url[:8] != "https://" {
		fail("only https:// URLs are allowed")
	}
	caPath := os.Getenv("CA_CERT_PATH")
	if caPath == "" {
		caPath = defaultCA
	}
	pem, err := os.ReadFile(caPath)
	if err != nil {
		fail("read CA bundle: %v", err)
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(pem) {
		fail("CA bundle contains no certificates")
	}

	client := &http.Client{
		Timeout: *timeout,
		Transport: &http.Transport{TLSClientConfig: &tls.Config{
			MinVersion: tls.VersionTLS12,
			RootCAs:    roots,
		}},
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			if req.URL.Scheme != "https" {
				return fmt.Errorf("refusing redirect to %s", req.URL.Scheme)
			}
			if len(via) >= 10 {
				return fmt.Errorf("too many redirects")
			}
			return nil
		},
	}
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		fail("request: %v", err)
	}
	req.Header.Set("User-Agent", "MinUIPlus-Updater/1")
	resp, err := client.Do(req)
	if err != nil {
		fail("request failed: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		fail("server returned %s", resp.Status)
	}
	if *release {
		if flag.NArg() != 1 {
			fail("-release writes to stdout and does not accept OUTPUT")
		}
		fields, err := normalizeRelease(resp.Body)
		if err != nil {
			fail("%v", err)
		}
		fmt.Print(fields)
		return
	}

	var out io.Writer = os.Stdout
	var file *os.File
	if flag.NArg() == 2 && flag.Arg(1) != "-" {
		file, err = os.Create(flag.Arg(1))
		if err != nil {
			fail("create output: %v", err)
		}
		out = file
	}
	_, copyErr := io.Copy(out, resp.Body)
	if file != nil {
		if copyErr == nil {
			copyErr = file.Sync()
		}
		if closeErr := file.Close(); copyErr == nil {
			copyErr = closeErr
		}
		if copyErr != nil {
			_ = os.Remove(flag.Arg(1))
		}
	}
	if copyErr != nil {
		fail("download: %v", copyErr)
	}
}
