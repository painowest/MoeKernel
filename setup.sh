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

gcc_() {
	GCC_64_DIR="$HOME/tc/aarch64-linux-android-14.0"
	GCC_32_DIR="$HOME/tc/arm-linux-androideabi-14.0"

	if ! [ -d "${GCC_64_DIR}" ]; then
		echo "gcc not found! Cloning to ${GCC_64_DIR}..."
		if ! git clone --depth=1 -b 14 https://github.com/ZyCromerZ/aarch64-zyc-linux-gnu ${GCC_64_DIR}; then
			echo "Cloning failed! Aborting..."
			exit 1
		fi
	fi

	if ! [ -d "${GCC_32_DIR}" ]; then
		echo "gcc_32 not found! Cloning to ${GCC_32_DIR}..."
		if ! git clone --depth=1 -b 14 https://github.com/ZyCromerZ/arm-zyc-linux-gnueabi ${GCC_32_DIR}; then
			echo "Cloning failed! Aborting..."
			exit 1
		fi
	fi
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
gcc_
kernelsu
kernel
