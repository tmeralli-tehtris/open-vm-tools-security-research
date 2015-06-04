#!/bin/bash

#Run this program after extracting the .deb in order to add the missing librairies and missing configuration files
rep="vmw"

# Copy $1 to $2 and creates the parent directories
copie_dir ()
{
        [ -e "${2}" ] && return
        rep_base=$(dirname "${2}")
        [ -d "${rep_base}" ] || {
                echo "++ mkdir -p ${rep_base}"
                mkdir -p "${rep_base}"
        }
        echo "+ cp -a $1 $2"
        cp -a "$1" "$2"
}

# Copy $1 to $2 + librairies
copie_ldd ()
{
        local src dest file f f_link
        src="$1"
        dest="$2"
        [ -e "${dest}" ] && return
        file=( $(ldd "$src" | awk '{print $3}' | grep '^/') )
        file=( "${file[@]}" $(ldd "$src" | grep '/' | grep -v '=>' | awk '{print $1}') )
        for f in "${file[@]}"
        do
                f_link=$(readlink -f "$f")
                copie_dir "$f_link" "${rep}${f}"
        done
        copie_dir "$src" "${dest}"
}
if [ -e $rep ]
then
	for prog in "$rep/bin/vmtoolsd" "/bin/bash" "/usr/bin/which" "/bin/date" "/bin/sh" "/usr/bin/dirname" "/usr/bin/basename" "/bin/sed" "/usr/bin/expr" "/sbin/shutdown" "/sbin/reboot" "/sbin/poweroff" 
	do
        	prog=$(which "$prog")
	        prog_real=$(readlink -f "$prog")
	        copie_ldd "$prog_real" "${rep}${prog}"
	done

	#cp the plugins to the right directory for chroot purposes
	cp -a /lib/x86_64-linux-gnu/libnss_compat* $rep/lib/x86_64-linux-gnu/
	cp -a /lib/x86_64-linux-gnu/libnsl* $rep/lib/x86_64-linux-gnu/
	cp -a /usr/lib/libdumbnet* $rep/usr/lib
	mkdir -p $rep/var
	mkdir -p $rep/var/log
	mkdir -p $rep/dev
	cp -a /dev/null $rep/dev
	hostname > $rep/etc/vmware-tools/fqdn.conf
#	ln -f /etc/localtime $rep/etc/localtime
else
	echo "vmw not found"
fi
