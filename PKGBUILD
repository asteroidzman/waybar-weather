# Maintainer: ralf <ralf.wierzbicki@gmail.com>
pkgname=waybar-weather
pkgver=1.0.0
pkgrel=1
pkgdesc='waybar CFFI plugin for weather (condition icon + temperature, hourly/daily popover)'
arch=('x86_64')
url='https://github.com/asteroidzman/waybar-weather'
license=('MIT')
depends=('waybar' 'gtk3' 'glib2' 'json-glib' 'gtk-layer-shell' 'curl')
makedepends=('pkgconf' 'git')
source=("git+$url.git#tag=$pkgver")
sha256sums=('SKIP')

build() {
  cd "$pkgname"
  make
}

package() {
  cd "$pkgname"
  make DESTDIR="$pkgdir" PREFIX=/usr/lib/waybar DATADIR=/usr/share/waybar-weather install
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
