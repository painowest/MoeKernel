#!/bin/bash

# install packages
packages() {
	sudo apt install bc ccache neovim -y
}

# configure clang
clang() {
	git clone https://github.com/moekernel/clang
	cd clang
	chmod +x clang.sh
	./clang.sh
	cd ../
	rm -rf clang
}

kernelsu() {
	./ksu_update.sh -t stable
}

# idk lol.
kernel() {
	make mrproper
}

packages
clang
kernelsu
kernel
