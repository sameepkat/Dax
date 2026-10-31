# Maintainer: Sameep Kathayat <sameepsk2@gmail.com>
pkgname=dax-shell
pkgver=1.0.0
pkgrel=1
pkgdesc="Customizable Unix shell with interactive features and Janet scripting"
arch=('x86_64' 'i686' 'aarch64')
url="https://github.com/sameepkat/Dax"
license=('GPL3' 'MIT')
depends=('readline')
makedepends=('git' 'gcc' 'make')
source=("git+https://github.com/sameepkat/Dax.git#tag=v${pkgver}")
sha256sums=('SKIP')

build() {
    cd "$srcdir/Dax"
    make VERSION="${pkgver}"
}

package() {
    cd "$srcdir/Dax"
    make DESTDIR="$pkgdir" PREFIX=/usr install
}
