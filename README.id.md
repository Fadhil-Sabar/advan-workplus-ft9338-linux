# Dukungan fingerprint FT9201/FT9338 untuk Linux

[English](README.md)

Proyek ini menyediakan dukungan Linux untuk pembaca sidik jari USB FocalTech
dengan ID `2808:9338`. Driver terintegrasi dengan
[libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) dan `fprintd`,
sehingga sensor dapat digunakan dari pengaturan desktop, PAM, dan command line.

## Status

Enrollment dan verifikasi sudah bekerja end-to-end pada perangkat yang diuji.

> **Disclaimer hardware:** driver ini dikembangkan dan divalidasi secara khusus
> menggunakan fingerprint reader bawaan laptop **Advan Workplus Ryzen 5 6600H**.
> Kompatibilitas ditentukan oleh USB ID dan revisi sensor, bukan hanya nama produk
> FT9201. Keberhasilan pada revisi Advan Workplus atau laptop lain tidak dijamin.

| Bagian | Nilai yang diuji |
| --- | --- |
| Komputer yang diuji | Advan Workplus Ryzen 5 6600H |
| USB ID | `2808:9338` |
| Chip sensor | `0x6893` / keluarga FT9338W |
| Gambar | 64 x 80 piksel, 500 DPI |
| Arsitektur host | x86-64 |
| Distribusi yang diuji | Fedora |
| Integrasi | libfprint, fprintd, KDE/GNOME, PAM |

Hasil verifikasi akhir pada mesin pengujian:

```text
Verify result: verify-match (done)
```

Repositori ini menargetkan perangkat tersebut. Product ID atau revisi sensor
FT9201 lain mungkin memakai protokol berbeda dan tidak otomatis didukung.

### Dukungan distribusi

Sejauh ini hanya Fedora x86-64 yang sudah diverifikasi langsung pada hardware.
Alur build dan instalasi seharusnya juga dapat digunakan pada distribusi x86-64
konvensional lain, tetapi kombinasi tersebut masih belum terverifikasi.

| Distribusi | Status saat ini |
| --- | --- |
| Fedora/Nobara | Fedora sudah diuji; Nobara diperkirakan memakai alur yang sama |
| Debian/Ubuntu | Diperkirakan bekerja; nama dependensi tersedia di bawah, tetapi masih perlu pengujian hardware |
| Arch/Manjaro | Diperkirakan bekerja setelah memasang paket yang setara; belum diverifikasi |
| openSUSE | Diperkirakan bekerja setelah memasang paket `-devel` yang setara; belum diverifikasi |
| NixOS, Alpine, sistem immutable, atau sistem tanpa systemd | Installer saat ini perlu disesuaikan |
| ARM/AArch64 | Tidak didukung karena matcher vendor berupa DLL x86-64 |

Installer saat ini mengasumsikan penggunaan systemd, udev,
`/etc/udev/rules.d`, `/usr/lib/libfprint-2`, dan `/usr/local/lib/ft9201`.
Source driver tidak terlalu terikat pada distribusi, tetapi layout lain perlu
menyesuaikan `scripts/install.sh` atau membuat paket native untuk distribusinya.

## Instalasi cepat

Repositori ini sudah memiliki installer side-by-side. File libfprint bawaan
distribusi tidak ditimpa.

### 1. Pasang dependensi build

Fedora:

```bash
sudo dnf install git meson ninja-build gcc curl cabextract python3 \
  glib2-devel gusb-devel nss-devel pixman-devel libgudev-devel systemd-devel
```

Padanan untuk Debian/Ubuntu:

```bash
sudo apt install git meson ninja-build gcc curl cabextract python3 \
  libglib2.0-dev libgusb-dev libnss3-dev libpixman-1-dev \
  libgudev-1.0-dev libsystemd-dev
```

### 2. Ambil file vendor yang dibutuhkan

Jalankan dari checkout repositori ini:

```bash
./scripts/fetch-blobs.sh
```

Skrip mengunduh matcher FocalTech dari driver Windows bertanda tangan dan
menghasilkan header firmware yang saat ini masih dibutuhkan source tree. Tidak
ada binary proprietary yang disimpan di repositori ini.

### 3. Build

```bash
./scripts/build.sh
```

Perintah ini mengambil versi libfprint yang sudah diuji, mendaftarkan driver
FT9201, lalu membangun library beserta contoh enrollment dan verification.

### 4. Pasang untuk fprintd

```bash
sudo ./scripts/install.sh
```

Installer akan:

- memasang libfprint khusus di `/usr/local/lib/ft9201`;
- memasang vendor matcher beserta prepared image yang aman secara W^X;
- memasang aturan udev untuk sensor;
- menambahkan systemd drop-in agar hanya `fprintd` yang menggunakan library
  khusus tersebut; dan
- memuat ulang udev serta me-restart `fprintd`.

Libfprint milik distribusi tetap tidak berubah.

### 5. Daftarkan sidik jari

```bash
fprintd-enroll
```

Enrollment dapat membutuhkan hingga 18 sampel yang diterima. Lanjutkan sampai
keluar `enroll-completed`; beberapa baris `enroll-stage-passed` saja belum
berarti sidik jari sudah tersimpan.

Agar sampel konsisten:

- sentuhkan jari, lalu angkat sepenuhnya sebelum sampel berikutnya;
- geser posisi sedikit untuk merekam bagian tengah dan tepi;
- pastikan sensor serta jari bersih dan kering; dan
- jangan terus menekan area jari yang persis sama.

### 6. Verifikasi

```bash
fprintd-verify
```

Setup yang berhasil akan berakhir dengan:

```text
Verify result: verify-match (done)
```

Sesudah itu autentikasi fingerprint dapat diaktifkan melalui pengaturan user KDE
atau GNOME.

## Menguji tanpa instalasi

Setelah build, contoh bawaan libfprint dapat dijalankan langsung:

```bash
sudo env \
  FT9201_ENGINE_DLL="$PWD/blobs/ftWbioEngineAdapter.dll" \
  LD_LIBRARY_PATH="$PWD/libfprint/build/libfprint" \
  "$PWD/libfprint/build/examples/enroll"
```

Kemudian verifikasi dengan:

```bash
sudo env \
  FT9201_ENGINE_DLL="$PWD/blobs/ftWbioEngineAdapter.dll" \
  LD_LIBRARY_PATH="$PWD/libfprint/build/libfprint" \
  "$PWD/libfprint/build/examples/verify"
```

Template yang dibuat oleh contoh yang dijalankan sebagai root adalah milik root
dan terpisah dari enrollment fprintd milik user biasa. Gunakan
`fprintd-enroll` untuk integrasi desktop dan PAM.

## Cara kerjanya

Alur aktif untuk perangkat `2808:9338` adalah:

1. Menginisialisasi USB bridge dan mengenali sensor keluarga FT9338W.
2. Melakukan polling sampai jari terdeteksi.
3. Mengambil satu frame 5120 byte, yaitu gambar grayscale 64 x 80.
4. Mengirim frame ke `ftWbioEngineAdapter.dll` milik FocalTech melalui lapisan
   kompatibilitas PE/WinBio native di dalam proses.
5. Mengembalikan hasil enrollment atau pencocokan melalui API libfprint normal.

Wine maupun instalasi Windows tidak diperlukan. Loader menyiapkan file-backed
image dengan halaman kode read/execute dan data writable yang private
read/write. Karena itu loader tetap kompatibel dengan hardening
`MemoryDenyWriteExecute` milik fprintd dan SELinux; fitur keamanan tersebut tidak
perlu dimatikan.

Jalur capture aktif `2808:9338` tidak mengunggah firmware MCU lama. Header
firmware yang dihasilkan masih menjadi dependensi build karena source tree masih
memuat jalur bring-up sebelumnya.

Riwayat investigasi dan detail protokol tersedia di
[FT9201-9338-WRAP-UP.md](FT9201-9338-WRAP-UP.md). Catatan umum untuk mengadaptasi
metode ini ke perangkat lain tersedia di [PORTING.md](PORTING.md).

## Troubleshooting

### `enroll-remove-and-retry` berulang kali

Angkat jari sepenuhnya di antara sampel. Jika masih berulang, bersihkan sensor
dengan kain microfiber kering dan keringkan jari. Saat pengujian, minyak pada
sensor menyebabkan loop retry panjang walaupun proses capture sebenarnya sudah
berfungsi.

### `NoEnrolledPrints` ketika verifikasi

Enrollment belum mencapai commit akhir atau dilakukan sebagai user lain.
Jalankan kembali `fprintd-enroll` sebagai user yang sama dan tunggu sampai
`enroll-completed`.

### `enroll-unknown-error` atau fprintd terputus

Periksa log service:

```bash
sudo journalctl -u fprintd --since "5 minutes ago" --no-pager
```

Pasang ulang build terbaru dan restart daemon:

```bash
sudo ./scripts/install.sh
sudo systemctl restart fprintd
```

### SELinux melaporkan write ke `/memfd:ftengine (deleted)`

Pesan itu berasal dari loader in-memory versi lama. Build ulang dan jalankan
installer kembali agar `ftWbioEngineAdapter.dll.image` dibuat dan dipasang.
Loader saat ini menggunakan file-backed image yang aman secara W^X.

## Uninstall

```bash
sudo ./scripts/install.sh --uninstall
```

Perintah ini menghapus library side-by-side, file matcher, aturan udev, dan
systemd drop-in. Libfprint bawaan tidak perlu dipulihkan karena sejak awal tidak
pernah diganti.

## Komponen proprietary

Repositori tidak mendistribusikan binary FocalTech. Skrip fetch mengambil:

| File | Kegunaan | Sumber |
| --- | --- | --- |
| `ftWbioEngineAdapter.dll` | Engine enrollment dan matching | Driver Windows FocalTech bertanda tangan dari Microsoft Update Catalog |
| `src/ft9201_fw.h` | Header firmware untuk build jalur lama | Diekstrak dari paket Linux FocalTech yang diarsipkan komunitas |

Matcher bersifat proprietary dan hanya tersedia untuk x86-64. Driver serta
compatibility loader yang terbuka tidak dapat memperbaiki masalah di dalam
algoritma vendor.

### Referensi driver Windows

Matcher yang digunakan proyek ini berasal dari driver biometric Windows
FocalTech yang bertanda tangan. `scripts/fetch-blobs.sh` mengambil paketnya
langsung dari CDN Windows Update milik Microsoft:

- [Pencarian Microsoft Update Catalog: FocalTech Electronics Biometric](https://www.catalog.update.microsoft.com/Search.aspx?q=FocalTech%20Electronics%20Biometric)
- [Microsoft CAB persis yang digunakan skrip fetch](https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2023/05/fd237921-f610-43de-b77c-f1685416480b_710b4d2c8dd2e80043e371b91bebb1721b5163a0.cab)

Paket CAB hanya digunakan sebagai sumber `ftWbioEngineAdapter.dll`. Paket driver
Windows tersebut tidak perlu dan tidak didukung untuk dipasang langsung di
Linux.

## Kredit

- [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) sebagai
  framework fingerprint Linux.
- [banianitc/ft9201-fingerprint-driver](https://github.com/banianitc/ft9201-fingerprint-driver)
  untuk riset protokol FT9201 sebelumnya.
- [mrrbrilliant/ft9201-static](https://github.com/mrrbrilliant/ft9201-static)
  untuk arsip paket Linux FocalTech yang digunakan sebagai referensi.
- [uunicorn/synaWudfBioUsb-sandbox](https://github.com/uunicorn/synaWudfBioUsb-sandbox)
  dan karya terkait untuk teknik tracing driver biometrik Windows.

## Lisensi

Driver, loader, dan skrip dalam repositori ini menggunakan lisensi
LGPL-2.1-or-later agar sesuai dengan libfprint. DLL dan firmware FocalTech tetap
menjadi milik pemiliknya serta tidak dilisensikan ulang atau didistribusikan di
sini.
