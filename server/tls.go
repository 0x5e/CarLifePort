package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"time"
)

const (
	selfSignedCertFileName = "self-signed.crt"
	selfSignedKeyFileName  = "self-signed.key"
)

// certificateDir is where the generated certificate is cached. It sits outside the
// repository so it survives rebuilds and does not depend on the working directory.
func certificateDir() string {
	if dir, err := os.UserCacheDir(); err == nil {
		return filepath.Join(dir, "carlifeport")
	}
	return filepath.Join(os.TempDir(), "carlifeport")
}

// selfSignedCertificate returns the certificate and key the HTTPS listener must use,
// generating them in dir when needed.
//
// The certificate is self-signed, so the browser rejects it until the page is opened once
// and the warning is accepted. It is still worth serving https: the browser client only
// gets a decoder in a secure context, and WebCodecs is unavailable over plain http
// everywhere except localhost. No certificate authority is involved, which is all this
// local page needs.
func selfSignedCertificate(dir string) (string, string, error) {
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return "", "", err
	}
	certPath := filepath.Join(dir, selfSignedCertFileName)
	keyPath := filepath.Join(dir, selfSignedKeyFileName)

	hosts := certificateHosts()
	if certificateCovers(certPath, hosts) {
		return certPath, keyPath, nil
	}
	if err := writeSelfSignedCertificate(certPath, keyPath, hosts); err != nil {
		return "", "", err
	}
	return certPath, keyPath, nil
}

// certificateHosts lists the names the certificate must be valid for: the loopback names
// plus every interface address a viewer URL is printed for.
func certificateHosts() []string {
	hosts := []string{"localhost", "127.0.0.1", "::1"}
	for _, entry := range interfaceAddresses() {
		hosts = append(hosts, entry.host)
	}
	return hosts
}

// certificateCovers reports whether the cached certificate is still usable: it must not be
// close to expiring and must be valid for every host. A machine that joins a different
// network therefore gets a fresh certificate instead of an address mismatch.
func certificateCovers(certPath string, hosts []string) bool {
	raw, err := os.ReadFile(certPath)
	if err != nil {
		return false
	}
	block, _ := pem.Decode(raw)
	if block == nil {
		return false
	}
	cert, err := x509.ParseCertificate(block.Bytes)
	if err != nil {
		return false
	}
	if time.Now().Add(24 * time.Hour).After(cert.NotAfter) {
		return false
	}
	for _, host := range hosts {
		if cert.VerifyHostname(host) != nil {
			return false
		}
	}
	return true
}

func writeSelfSignedCertificate(certPath, keyPath string, hosts []string) error {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return err
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return err
	}

	template := x509.Certificate{
		SerialNumber: serial,
		Subject:      pkix.Name{CommonName: "CarLifePort"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().AddDate(10, 0, 0),
		KeyUsage:     x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	for _, host := range hosts {
		if ip := net.ParseIP(host); ip != nil {
			template.IPAddresses = append(template.IPAddresses, ip)
			continue
		}
		template.DNSNames = append(template.DNSNames, host)
	}

	der, err := x509.CreateCertificate(rand.Reader, &template, &template, &key.PublicKey, key)
	if err != nil {
		return err
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		return err
	}

	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})
	if err := os.WriteFile(certPath, certPEM, 0o644); err != nil {
		return err
	}
	return os.WriteFile(keyPath, keyPEM, 0o600)
}
