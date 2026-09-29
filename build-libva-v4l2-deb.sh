#!/usr/bin/env bash
#
# build-libva-v4l2-deb.sh
# Construit le paquet Debian de libva-v4l2 (VA-API sur Qualcomm Iris / V4L2 stateful)
# à partir de https://github.com/Trak-unixbased/libva-v4l2
#
# Le dépôt fournit déjà un répertoire debian/ (debhelper 13, buildsystem meson,
# FastCV désactivé) : ce script se contente d'orchestrer la construction, en
# reproduisant la CI upstream (.github/workflows/debian.yml).
#
# Cible : Debian / Ubuntu arm64 (le paquet est déclaré "Architecture: arm64").
#
# Usage : ./build-libva-v4l2-deb.sh [options]
#   -r REF      branche, tag ou commit à construire      (défaut : master)
#   -u URL      URL du dépôt git                          (défaut : dépôt Trak-unixbased)
#   -o DIR      répertoire de sortie des paquets          (défaut : ./dist)
#   -w DIR      répertoire de travail                     (défaut : mktemp)
#   -d DIST     distribution inscrite dans le .changes    (défaut : codename de l'hôte)
#   -s          suffixer la version (+gitAAAAMMJJ.<sha>) pour distinguer le build local
#   -n          ne pas installer les dépendances (déjà présentes)
#   -p          purger le méta-paquet de build-deps après construction
#   -l          ne pas lancer lintian
#   -k          conserver le répertoire de travail
#   -h          aide

set -Eeuo pipefail

# --- Paramètres par défaut ---------------------------------------------------
REPO_URL="https://github.com/Trak-unixbased/libva-v4l2.git"
REF="master"
OUTDIR="$(pwd)/dist"
WORKDIR=""
LOCAL_VERSION=0
INSTALL_DEPS=1
PURGE_DEPS=0
RUN_LINTIAN=1
KEEP_WORKDIR=0
DIST=""
PKG="libva-v4l2"

# --- Fonctions utilitaires ---------------------------------------------------
log()  { printf '\033[1;34m[*]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[+]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

cleanup() {
    local rc=$?
    if [[ -n "${WORKDIR}" && -d "${WORKDIR}" && ${KEEP_WORKDIR} -eq 0 ]]; then
        rm -rf -- "${WORKDIR}"
    elif [[ -n "${WORKDIR}" ]]; then
        log "Répertoire de travail conservé : ${WORKDIR}"
    fi
    [[ ${rc} -ne 0 ]] && warn "Échec (code ${rc})."
    exit "${rc}"
}
trap cleanup EXIT
trap 'die "Erreur ligne ${LINENO} : ${BASH_COMMAND}"' ERR

# --- Arguments ---------------------------------------------------------------
while getopts ":r:u:o:w:d:snplkh" opt; do
    case "${opt}" in
        r) REF="${OPTARG}" ;;
        u) REPO_URL="${OPTARG}" ;;
        o) OUTDIR="$(realpath -m "${OPTARG}")" ;;
        w) WORKDIR="$(realpath -m "${OPTARG}")"; KEEP_WORKDIR=1 ;;
        d) DIST="${OPTARG}" ;;
        s) LOCAL_VERSION=1 ;;
        n) INSTALL_DEPS=0 ;;
        p) PURGE_DEPS=1 ;;
        l) RUN_LINTIAN=0 ;;
        k) KEEP_WORKDIR=1 ;;
        h) usage ;;
        :) die "L'option -${OPTARG} requiert un argument." ;;
        *) die "Option inconnue : -${OPTARG} (voir -h)." ;;
    esac
done

# --- Vérifications préalables ------------------------------------------------
command -v dpkg >/dev/null || die "dpkg introuvable : système Debian/Ubuntu requis."

HOST_ARCH="$(dpkg --print-architecture)"
[[ "${HOST_ARCH}" == "arm64" ]] || die "Architecture hôte '${HOST_ARCH}' : le paquet est arm64 uniquement.
    Lancez ce script sur une machine arm64 (ou dans un conteneur arm64, ex. :
    docker run --rm -it --platform linux/arm64 -v \"\$PWD\":/out ubuntu:noble)."

if [[ -z "${DIST}" ]]; then
    # shellcheck disable=SC1091
    DIST="$(. /etc/os-release && echo "${VERSION_CODENAME:-unstable}")"
fi

if [[ ${EUID} -eq 0 ]]; then
    SUDO=""
else
    command -v sudo >/dev/null || die "sudo requis pour installer les dépendances (ou lancer en root)."
    SUDO="sudo"
fi

# --- Outils de packaging -----------------------------------------------------
if [[ ${INSTALL_DEPS} -eq 1 ]]; then
    log "Installation des outils de packaging…"
    export DEBIAN_FRONTEND=noninteractive
    ${SUDO} apt-get update -qq
    ${SUDO} apt-get install -y --no-install-recommends \
        ca-certificates git build-essential devscripts equivs lintian fakeroot
fi

for bin in git dpkg-buildpackage dpkg-parsechangelog; do
    command -v "${bin}" >/dev/null || die "Commande manquante : ${bin} (relancer sans -n)."
done

# --- Récupération des sources ------------------------------------------------
[[ -n "${WORKDIR}" ]] || WORKDIR="$(mktemp -d -t "${PKG}-build.XXXXXX")"
SRCDIR="${WORKDIR}/${PKG}"
mkdir -p "${WORKDIR}" "${OUTDIR}"

if [[ -d "${SRCDIR}/.git" ]]; then
    log "Sources déjà présentes, nettoyage…"
    git -C "${SRCDIR}" reset -q --hard
    git -C "${SRCDIR}" clean -qfdx
else
    log "Clonage de ${REPO_URL}…"
    git init -q "${SRCDIR}"
    git -C "${SRCDIR}" remote add origin "${REPO_URL}"
fi

log "Vérification de la référence '${REF}'…"
if ! git ls-remote --exit-code --heads --tags "${REPO_URL}" "${REF}" >/dev/null 2>&1; then
    if [[ ! "${REF}" =~ ^[0-9a-fA-F]{7,40}$ ]]; then
        warn "Référence '${REF}' introuvable sur ${REPO_URL}. Références disponibles :"
        git ls-remote --heads --tags "${REPO_URL}" | awk '{print "      " $2}' | sed 's#refs/##; s#\^{}##' | sort -u >&2
        die "Choisissez une branche ou un tag existant (option -r), ou un SHA de commit."
    fi
    log "'${REF}' ressemble à un SHA de commit, tentative de récupération directe."
fi

log "Récupération de la référence '${REF}'…"
git -C "${SRCDIR}" fetch -q --depth 1 origin "${REF}"
git -C "${SRCDIR}" checkout -q --detach FETCH_HEAD
SHA="$(git -C "${SRCDIR}" rev-parse --short=8 HEAD)"
ok "Commit : ${SHA}"

cd "${SRCDIR}"
[[ -f debian/control && -f debian/rules ]] || die "Pas de répertoire debian/ valide dans cette référence."

# --- Version -----------------------------------------------------------------
if [[ ${LOCAL_VERSION} -eq 1 ]]; then
    command -v dch >/dev/null || die "dch (devscripts) requis pour -s."
    BASE_VER="$(dpkg-parsechangelog -S Version)"
    NEW_VER="${BASE_VER}+git$(date +%Y%m%d).${SHA}"
    log "Version locale : ${NEW_VER}"
    DEBFULLNAME="${DEBFULLNAME:-$(git config user.name 2>/dev/null || echo "Local Builder")}" \
    DEBEMAIL="${DEBEMAIL:-$(git config user.email 2>/dev/null || echo "builder@localhost")}" \
        dch --newversion "${NEW_VER}" --distribution UNRELEASED --force-distribution \
            "Construction locale depuis ${REPO_URL} @ ${SHA}."
fi
VERSION="$(dpkg-parsechangelog -S Version)"

# --- Dépendances de construction ---------------------------------------------
if [[ ${INSTALL_DEPS} -eq 1 ]]; then
    log "Installation des Build-Depends (mk-build-deps)…"
    ${SUDO} mk-build-deps --install --remove \
        --tool 'apt-get -y --no-install-recommends' debian/control
    # mk-build-deps laisse parfois .buildinfo/.changes dans le répertoire courant
    rm -f -- "${PKG}"-build-deps_*
else
    log "Vérification des Build-Depends…"
    dpkg-checkbuilddeps || die "Dépendances de construction manquantes (relancer sans -n)."
fi

# --- Construction ------------------------------------------------------------
log "Construction de ${PKG} ${VERSION} pour ${DIST} (binaire uniquement)…"
# La distribution du changelog ("unstable") est rejetée par le lintian d'Ubuntu :
# on la remplace dans le .changes, comme le fait la CI upstream (-DDistribution=noble).
dpkg-buildpackage -us -uc -b -j"$(nproc)" --changes-option=-DDistribution="${DIST}"

# --- Contrôles ---------------------------------------------------------------
CHANGES="$(ls -1 "${WORKDIR}/${PKG}_${VERSION#*:}_"*.changes 2>/dev/null | head -n1 || true)"
[[ -n "${CHANGES}" ]] || die "Fichier .changes introuvable : la construction a-t-elle abouti ?"

# --- Collecte des artefacts --------------------------------------------------
log "Copie des artefacts vers ${OUTDIR}…"
shopt -s nullglob
ARTIFACTS=( "${WORKDIR}"/*.deb "${WORKDIR}"/*.ddeb "${WORKDIR}"/*.buildinfo "${WORKDIR}"/*.changes )
shopt -u nullglob
[[ ${#ARTIFACTS[@]} -gt 0 ]] || die "Aucun artefact produit."
cp -v -- "${ARTIFACTS[@]}" "${OUTDIR}/"

DEB="$(ls -1 "${OUTDIR}/${PKG}_${VERSION#*:}_arm64.deb")"
( cd "${OUTDIR}" && sha256sum -- *.deb *.ddeb 2>/dev/null > SHA256SUMS || true )

# lintian après la copie : un échec d'analyse ne fait plus perdre le build
if [[ ${RUN_LINTIAN} -eq 1 ]] && command -v lintian >/dev/null; then
    log "Analyse lintian…"
    lintian --fail-on error "${OUTDIR}/$(basename "${CHANGES}")" \
        || die "lintian a relevé des erreurs (paquets conservés dans ${OUTDIR})."
fi

echo
dpkg-deb --info "${DEB}"
echo
dpkg-deb --contents "${DEB}"

# --- Nettoyage optionnel des build-deps --------------------------------------
if [[ ${PURGE_DEPS} -eq 1 && ${INSTALL_DEPS} -eq 1 ]]; then
    log "Purge du méta-paquet ${PKG}-build-deps…"
    ${SUDO} apt-get purge -y "${PKG}-build-deps" || true
    ${SUDO} apt-get autoremove -y --purge || true
fi

echo
ok "Paquet prêt : ${DEB}"
cat <<EOF

Installation :   sudo apt install ${DEB}
Vérification :   vainfo --display drm --device /dev/dri/renderD128
Rappel : le noyau doit intégrer le driver Iris stateful + les correctifs de patches/
         (non inclus dans le .deb).
EOF
