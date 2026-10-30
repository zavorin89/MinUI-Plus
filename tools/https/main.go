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
	"strings"
	"time"
)

const defaultCA = "/mnt/SDCARD/.system/res/cacert.pem"

func fail(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "https: "+format+"\n", args...)
	os.Exit(1)
}

func main() {
	timeout := flag.Duration("t", 30*time.Second, "total request timeout")
	release := flag.Bool("release", false, "normalize a GitHub release for the updater")
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
		var parsed struct {
			TagName string `json:"tag_name"`
			Body    string `json:"body"`
			Assets  []struct {
				Name               string `json:"name"`
				BrowserDownloadURL string `json:"browser_download_url"`
				Digest             string `json:"digest"`
				Size               int64  `json:"size"`
			} `json:"assets"`
		}
		if err := json.NewDecoder(resp.Body).Decode(&parsed); err != nil {
			fail("decode GitHub release: %v", err)
		}
		for _, asset := range parsed.Assets {
			if strings.HasPrefix(asset.Name, "MinUIPlus-") && strings.HasSuffix(asset.Name, ".zip") {
				digest := strings.TrimPrefix(asset.Digest, "sha256:")
				fmt.Printf("%s\n%s\n%s\n%s\n%d\n%s\n", parsed.TagName, asset.Name, asset.BrowserDownloadURL, digest, asset.Size, parsed.Body)
				return
			}
		}
		fail("GitHub release has no MinUIPlus ZIP asset")
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
