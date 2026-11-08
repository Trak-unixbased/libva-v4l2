#!/bin/bash
set -e

REPO_URL="https://github.com/Trak-unixbased/libva-v4l2"
SRC_DIR="libva-v4l2"
PKG_NAME="libva-v4l2-cix8180"
PKG_VERSION="1.0.0-$(date +%Y%m%d)"
ARCH="arm64"
DEB_DIR="${PKG_NAME}_${PKG_VERSION}_${ARCH}"

echo "=> Nettoyage de l'espace de travail..."
rm -rf $SRC_DIR $DEB_DIR
git clone $REPO_URL
cd $SRC_DIR

echo "=> Analyse statique et nettoyage du code (Debug/Clean)..."
# Utilisation de cppcheck pour identifier d'éventuelles fuites mémoire ou erreurs C
cppcheck --enable=all --inconclusive --force --inline-suppr -q .
# Optionnel : formatage propre du code
# find . -regex '.*\.\(c\|h\|cpp\|hpp\)' -exec clang-format -i {} +

echo "=> Application des flags d'optimisation pour CIX CD8180..."
# Le SoC CIX profite des instructions ARMv8.2 et du Link Time Optimization (LTO)
export CFLAGS="-O3 -march=armv8.2-a -mtune=generic -flto -pipe -fno-semantic-interposition -Wall -Wextra"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-Wl,-O1 -Wl,--as-needed -flto -Wl,-Bsymbolic"

echo "=> Configuration du build (Meson)..."
# Cibler le bon répertoire DRI pour Ubuntu aarch64
meson setup build --prefix=/usr --libdir=lib/aarch64-linux-gnu --buildtype=release -Ddefault_library=shared

echo "=> Compilation..."
ninja -C build

echo "=> Préparation de l'arborescence du paquet Debian..."
cd ..
mkdir -p ${DEB_DIR}/DEBIAN
# Installation dans un répertoire fakeroot (DESTDIR)
DESTDIR=$(pwd)/${DEB_DIR} ninja -C ${SRC_DIR}/build install

echo "=> Création du fichier DEBIAN/control..."
cat <<EOF > ${DEB_DIR}/DEBIAN/control
Package: $PKG_NAME
Version: $PKG_VERSION
Architecture: $ARCH
Maintainer: Trak.unixbased <trak.unixbased@gmail.com>
Depends: libva2, libdrm2
Section: video
Priority: optional
Description: Backend libva-v4l2 optimisé pour SoC CIX CD8180 (Orange Pi 6 Plus)
 Ce paquet contient le pilote V4L2 pour l'accélération matérielle VA-API.
 Compilé avec LTO et optimisations ARMv8.2-A pour Ubuntu 26.04 LTS aarch64.
EOF

echo "=> Construction du paquet Debian..."
dpkg-deb --build ${DEB_DIR}

echo "=> Succès ! Paquet généré : ${DEB_DIR}.deb"
