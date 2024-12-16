#!/bin/bash

link() {
	SRC=${1}
	TGT=${2}
	if [[ ! -d "${SRC}" ]]; then
		echo "ERROR: ${SRC} does not exist"
		return
	fi

	if [[ -d "${TGT}" ]]
	then
		if [[ "$(realpath ${TGT})" = "$(realpath ${SRC})" ]]
		then
			return
		else
			rm ${TGT}
		fi
	fi

	ln -s ${SRC} ${TGT}
}

link 4.18.0-240.10.1.el8.x86_64 4.18.0-240.22.1.el8_3.x86_64 
link 4.18.0-477.27.1.el8.x86_64 4.18.0-477.27.1.el8_8.x86_64
link 5.14.0-284.11.1.el9_2.x86_64 5.14.0-284.el9_2.x86_64
