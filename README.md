# libva-v4l2

**libva-v4l2** est un pilote d'arrière-plan (*backend driver*) pour l'API **VA-API** (Video Acceleration API) sous Linux. 

Il permet d'assurer l'interface entre l'API VA-API (utilisée par les lecteurs vidéo et navigateurs web) et l'API noyau Linux **V4L2 Memory-to-Memory (M2M) Stateless / Request API**, exploitée par les accélérateurs matériels (VPU) de nombreux processeurs embarqués (SoC).

---

## Sommaire

- [À quoi sert ce projet ?](#à-quoi-sert-ce-projet-)
- [Architecture & Fonctionnement](#architecture--fonctionnement)
- [Codecs et Plateformes pris en charge](#codecs-et-plateformes-pris-en-charge)
- [Prérequis](#prérequis)
  - [Matériel et Noyau](#matériel-et-noyau)
  - [Dépendances logicielles](#dépendances-logicielles)
- [Compilation et Installation](#compilation-et-installation)
  - [1. Cloner le dépôt](#1-cloner-le-dépôt)
  - [2. Configurer et compiler avec Meson](#2-configurer-et-compiler-avec-meson)
  - [3. Installer la bibliothèque](#3-installer-la-bibliothèque)
- [Configuration et Variables d'environnement](#configuration-et-variables-denvironnement)
  - [Déclaration du pilote VA-API](#déclaration-du-pilote-va-api)
  - [Sélection explicite des périphériques V4L2](#sélection-explicite-des-périphériques-v4l2)
  - [Compatibilité GStreamer](#compatibilité-gstreamer)
- [Validation et Mise en œuvre](#validation-et-mise-en-œuvre)
  - [Vérification avec vainfo](#vérification-avec-vainfo)
  - [Test de décodage avec FFmpeg / MPV](#test-de-décodage-avec-ffmpeg--mpv)
- [Dépannage fréquent](#dépannage-fréquent)
- [Licence et Origines](#licence-et-origines)

---

## À quoi sert ce projet ?

Sur de nombreuses cartes de développement et ordinateurs monocartes (SBCs basés sur Rockchip, Allwinner, NXP, etc.), le décodage vidéo matériel (VPU) est piloté dans le noyau Linux via les sous-systèmes **Video4Linux2 (V4L2)** et l'API **Request/M2M Stateless**.

Cependant, la plupart des applications multimédias de bureau (VLC, MPV, Chromium, Firefox, Kodi, GStreamer) n'implémentent pas directement l'API V4L2 Stateless et s'appuient plutôt sur des standards de plus haut niveau comme **VA-API**.

**libva-v4l2** sert de passerelle d'abstraction :
- Il expose une interface standard VA-API (`v4l2_drv_video.so`).
- Il traduit les ordres de décodage VA-API en commandes V4L2 Stateless / Request API pour le pilote noyau du SoC.
- Il décharge le processeur principal (CPU) en confiant le décodage vidéo lourd au VPU matériel.

---

## Architecture & Fonctionnement

```text
+-------------------------------------------------------+
|  Applications multimédias (MPV, VLC, FFmpeg, GStreamer)|
+-------------------------------------------------------+
                           |
                           v
               [ VA-API (libva.so) ]
                           |
                           v
          [ libva-v4l2 (v4l2_drv_video.so) ]
                           |
          (V4L2 M2M Request API / IOCTLs)
                           |
                           v
+-------------------------------------------------------+
| Noyau Linux : Pilotes V4L2 (Hantro, Cedrus, RKVDEC)    |
| Périphériques : /dev/videoX et /dev/mediaY            |
+-------------------------------------------------------+
                           |
                           v
        [ VPU Matériel du Processeur / SoC ]
```

---

## Codecs et Plateformes pris en charge

### Codecs
Selon la maturité du pilote noyau et du matériel cible, le backend prend en charge ou vise :
- **MPEG-2** (Simple / Main profile)
- **H.264 / AVC** (Baseline, Main, High profiles)
- **VP8**
- **VP9** (partiel ou selon version)
- **H.265 / HEVC** (sur les versions dérivées de *libva-v4l2-request*)

### Plateformes matérielles courantes
- **Rockchip** (RK3399, RK3328, RK3566, RK3588 avec pilotes *hantro* ou *rkvdec*).
- **Allwinner** (A64, H3, H5, H6 via le pilote noyau *cedrus*).
- **NXP i.MX** (i.MX8MQ avec pilote *hantro*).

---

## Prérequis

### Matériel et Noyau
1. Un SoC Linux doté d'un décodeur VPU géré par un pilote mainline V4L2.
2. Un noyau Linux récent (idéalement $\ge$ 5.10) compilé avec le support des APIs multimédia :
   - `CONFIG_MEDIA_SUPPORT=y`
   - `CONFIG_MEDIA_CONTROLLER=y`
   - `CONFIG_VIDEO_DEV=y`
   - `CONFIG_V4L2_MEM2MEM_DEV=y`
3. Présence des nœuds `/dev/media*` et `/dev/video*` accessibles en lecture/écriture par votre utilisateur (appartenance au groupe `video`).

### Dépendances logicielles

Sous Debian / Ubuntu / Armbian :
```bash
sudo apt update
sudo apt install -y \
    build-essential \
    meson \
    ninja-build \
    pkg-config \
    libva-dev \
    libdrm-dev \
    vainfo \
    v4l-utils
```

Sous Arch Linux ARM / Manjaro ARM :
```bash
sudo pacman -Syu --needed \
    base-devel \
    meson \
    ninja \
    pkgconf \
    libva \
    libdrm \
    libva-utils \
    v4l-utils
```

---

## Compilation et Installation

### 1. Cloner le dépôt

```bash
git clone https://github.com/Trak-unixbased/libva-v4l2.git
cd libva-v4l2
```

### 2. Configurer et compiler avec Meson

```bash
# Configuration du répertoire de compilation
meson setup build --prefix=/usr

# Compilation du pilote
meson compile -C build
```

### 3. Installer la bibliothèque

```bash
sudo meson install -C build
```

Le pilote compilé (généralement `v4l2_drv_video.so`) sera copié dans le répertoire officiel des pilotes VA-API (par exemple `/usr/lib/dri/` ou `/usr/lib/aarch64-linux-gnu/dri/`).

---

## Configuration et Variables d'environnement

### Déclaration du pilote VA-API

Pour forcer `libva` à employer ce backend :

```bash
# Indiquer le nom du pilote
export LIBVA_DRIVER_NAME=v4l2

# Si vous testez directement depuis le dossier build sans installer :
export LIBVA_DRIVERS_PATH="$(pwd)/build/src"
```

### Sélection explicite des périphériques V4L2

Par défaut, le pilote sonde le système pour identifier les décodeurs compatibles. Si plusieurs cartes vidéo ou média existent (ex: une webcam USB + le VPU interne), forcez les périphériques cibles :

```bash
# Identifier les contrôleurs médias disponibles
v4l2-ctl --list-devices

# Définir explicitement les chemins
export LIBVA_V4L2_VIDEO_PATH=/dev/video0
export LIBVA_V4L2_MEDIA_PATH=/dev/media0
```

### Compatibilité GStreamer

GStreamer utilise une liste blanche de pilotes autorisés pour VA-API. Activez le pilote V4L2 avec :

```bash
export GST_VAAPI_ALL_DRIVERS=1
```

---

## Validation et Mise en œuvre

### Vérification avec vainfo

Lancez la commande suivante pour contrôler que le pilote se charge et liste les profils matériels :

```bash
LIBVA_DRIVER_NAME=v4l2 vainfo
```

**Sortie attendue (exemple) :**
```text
vainfo: VA-API version: 1.x
vainfo: Driver version: v4l2 backend
vainfo: Supported profile and entrypoints
      VAProfileMPEG2Simple            : VAEntrypointVLD
      VAProfileMPEG2Main              : VAEntrypointVLD
      VAProfileH264ConstrainedBaseline: VAEntrypointVLD
      VAProfileH264Main               : VAEntrypointVLD
      VAProfileH264High               : VAEntrypointVLD
```

### Test de décodage avec FFmpeg / MPV

#### Avec MPV :
```bash
LIBVA_DRIVER_NAME=v4l2 mpv --hwdec=vaapi --vo=gpu video_test_h264.mp4
```

#### Avec FFmpeg :
```bash
LIBVA_DRIVER_NAME=v4l2 ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
    -i video_test_h264.mp4 -f null -
```

---

## Dépannage fréquent

1. **`vaGetDriverName() returns -1` ou pilote non trouvé :**
   - Vérifiez l'emplacement de `v4l2_drv_video.so`. Définissez `LIBVA_DRIVERS_PATH` vers son dossier d'installation.
2. **Permissions refusées (`Permission denied`) sur `/dev/video*` :**
   - Ajoutez votre utilisateur au groupe `video` :
     ```bash
     sudo usermod -aG video $USER
     ```
   - Déconnectez-vous puis reconnectez-vous.
3. **Échec d'allocation mémoire / DMABUF :**
   - Certains VPU nécessitent une réserve de mémoire contiguë (CMA - Contiguous Memory Allocator). Augmentez la taille CMA dans la ligne de commande du noyau (`/boot/cmdline.txt` ou `armbianEnv.txt`), par exemple :
     ```text
     cma=256M
     ```

---

## Licence et Origines

- Ce projet dérive des travaux initiaux de **Bootlin** (`libva-v4l2-request`) et des continuations communautaires (`mxsrc/libva-v4l2`).
- Distribué principalement sous licence **LGPL-2.1** (consultez le fichier `COPYING` ou `LICENSE` pour les conditions exactes).
# libva-v4l2

A VA-API backend for Qualcomm Iris, using the Linux V4L2 stateful M2M interface.
It provides hardware video decoding and H.264/HEVC encoding on **SC8280XP**, tested on
**Radxa Dragon Q8B**. The project is experimental and targets this platform;
it is not a generic backend for all V4L2 devices.

## Support

| Codec | Decoding | Encoding |
| --- | --- | --- |
| H.264 / AVC | Constrained Baseline, Main, High (8-bit) | Same profiles (8-bit) |
| H.265 / HEVC | Main, Main10 (8/10-bit) | Main (8-bit) |
| VP9 | Profiles 0 and 2 (8/10-bit) | — |

Supported video is progressive 4:2:0. Decoding has been tested with FFmpeg,
GStreamer, mpv, VLC, Chromium, and Kodi; H.264 encoding with FFmpeg, GStreamer,
and Sunshine. HEVC encoding is tested with FFmpeg and GStreamer.

- H.264 and HEVC encoding support CQP, CBR, and VBR, even dimensions from 128×128 to
  3840×2160, and I/P frames with one reference. I-frame requests produce IDR frames; B frames
  are unsupported.
  HEVC encoding currently uses Main tier; Main10 encoding is not implemented.
- Optional FastCV VPP provides CDSP scaling for linear NV12 with DMA-BUF support.
  Widths must be multiples of 16 and heights even. Color/bit-depth conversion
  and other VPP filters are unsupported.
- Compatible DMA-BUF paths avoid raw-frame copies. Applications that cache
  surfaces before decoding use a GPU copy by default.

## Performance

HEVC Main (8-bit) throughput on **Radxa Dragon Q8B — SC8280XP, Iris HFI Gen2**,
using FFmpeg 9.0.1 and GStreamer 1.28.6. Values are average frames per second.

| Pipeline | Operation | 720p (1280×720) | 1080p (1920×1080) | 4K (3840×2160) |
| --- | --- | ---: | ---: | ---: |
| FFmpeg VA-API | Decode | 1111 | 806 | 297 |
| FFmpeg VA-API | Encode | 1289 | 714 | 206 |
| GStreamer VA | Decode | 1088 | 823 | 278 |
| GStreamer VA | Encode | 1285 | 709 | 206 |
| GStreamer direct V4L2 | Decode | 1199 | 882 | 330 |
| GStreamer direct V4L2 | Encode | 1287 | 724 | 210 |

Synthetic I/P streams, GOP 60, VBR at 5 / 10 / 20 Mbps respectively. Encoding
includes CPU NV12 upload; FFmpeg encoding uses **`async_depth=4`**. Decoding waits
for completed frames without CPU download or rendering. Results are three-run
means, except 4K encoding, which uses one completed run; each run processes
9,000 / 6,000 / 2,400 frames respectively. Actual performance depends on content
and pipeline configuration.

## Requirements

- Linux with the Qualcomm Iris stateful driver and MSM DRM support.
- The changes supplied in [patches/](patches/), unless already present in your
  kernel: decode-order output, larger CAPTURE pools, and encoder deblocking
  controls. The patches are references to integrate into your kernel.
- Access to the Iris video devices and DRM render node.
- A C++17 compiler, Meson ≥ 0.61, Ninja, pkg-config, and development libraries
  for libva, libdrm, EGL, GLES 3.2, and GBM.
- For VPP: system FastCV and FastRPC libraries with a working CDSP runtime.

## Build and install

The Debian package CI builds one ARM64 package on Ubuntu Noble, then checks
installation and driver loading on Debian Trixie, Ubuntu Noble, and Ubuntu
Resolute. Tag builds publish the same package and debug symbols to a draft
GitHub Release after all checks pass. Hardware codec testing is separate.

On Arch Linux:

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf libva libdrm mesa libglvnd
python3 packaging/make-dist.py
cd packaging/arch
makepkg -si
```

The [PKGBUILD](packaging/arch/PKGBUILD) packages the userspace driver. After
editing packaged sources, regenerate the archive, update `sha256sums` using
`makepkg -g`, and refresh `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`.

Alternatively, build and install with Meson:

```sh
meson setup build --buildtype=release --prefix=/usr -Dlibdir=lib
meson compile -C build
sudo meson install -C build
```

FastCV VPP defaults to `-Dfastcv=disabled`. Use `-Dfastcv=auto` to enable it
when dependencies are available or `-Dfastcv=enabled` to require build dependencies.
Debian / Ubuntu packages explicitly disable FastCV for consistent distribution builds.
FastCV is loaded dynamically; missing runtime libraries disable only VPP.

Adjust `libdir` if your system uses a driver directory other than `/usr/lib/dri`.
Installation adds the MSM driver alias, so **`LIBVA_DRIVER_NAME` is not required**
on the target platform.

To check the installation (`vainfo` comes from `libva-utils` on Arch):

```sh
vainfo --display drm --device /dev/dri/renderD128
```

## Usage

Play a video with mpv:

```sh
mpv --hwdec=vaapi input.mp4
```

Encode to H.264 with FFmpeg:

```sh
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 -an \
  -vf format=nv12,hwupload -c:v h264_vaapi -profile:v high \
  -bf 0 -g 60 -rc_mode CBR -b:v 6M -async_depth 4 output.mp4
```

For HEVC, use `-c:v hevc_vaapi -profile:v main` in the command above.

For hardware decoding and encoding of a supported 8-bit input:

```sh
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i input.mp4 -an \
  -c:v h264_vaapi -bf 0 -qp 24 -async_depth 4 output.mp4
```

MP4 and raw H.264 output are supported; for Matroska, encode to raw H.264 first
and remux. FFmpeg may warn about unsupported packed headers because the firmware
produces the headers. Custom VUI/SEI metadata is not forwarded.

Scale a decoded NV12 video on CDSP:

```sh
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i input.mp4 -an \
  -vf scale_vaapi=w=1920:h=1080 -c:v h264_vaapi -bf 0 -qp 24 \
  -async_depth 4 output.mp4
```

For GStreamer, set `GST_VA_ALL_DRIVERS=1` to enable this driver's `va` elements:

```sh
GST_VA_ALL_DRIVERS=1 gst-launch-1.0 -e \
  videotestsrc num-buffers=120 ! \
  video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! \
  vah264enc rate-control=vbr bitrate=6000 target-percentage=100 \
    b-frames=0 ref-frames=1 target-usage=1 key-int-max=60 ! \
  'video/x-h264,profile=high' ! h264parse ! mp4mux ! filesink location=output.mp4
```

For Sunshine/Moonlight, use **H.264 / SDR**. HEVC and AV1 encoder probes may fail
while H.264 remains available.

## Configuration and development

| Variable | Purpose |
| --- | --- |
| `LIBVA_DRIVER_NAME=v4l2` | Explicitly select this backend. |
| `LIBVA_DRIVERS_PATH=/path/to/build` | Load an uninstalled build. |
| `IRIS_VAAPI_DEVICE` / `IRIS_VAAPI_ENCODER_DEVICE` | Override automatic video-device discovery. |
| `IRIS_VAAPI_COPY=gpu` or `cpu` | Select the copy path; default is `gpu`. |
| `IRIS_VAAPI_DEBUG=1` | Enable diagnostics. |

Use clang-format 22 and the checked-in `.clang-format`. Meson provides `format`
and `format-check` targets when clang-format is available; `-Dwerror=true`
enables compiler warnings as errors. Hardware tests and their run instructions
are in [tests/](tests/). Local research and historical results live in ignored
`tmp/` and are not required to build the driver.

## License

The backend is [MIT licensed](LICENSE). Bundled Linux kernel patches are
GPL-2.0-only; external test frameworks and media retain their own licenses.
