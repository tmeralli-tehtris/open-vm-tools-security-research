#!/bin/bash

mkdir -p ../proc
function check_file {
  file=$1
  if [ -e ..$file ]
    then
    #check if the file has changed
          if cmp -s "$file" "..$file"
            then
              echo "no changes have been made"
            else
              echo "changes have been made"
              cp -a $file ..$file
        fi
    else
      #copy the file
      echo "file was not created yet"
      cp -a $file ..$file
  fi
}
# we cannot use inotify since it is a kernel file


while true ; do
        for file in /proc/uptime /etc/localtime
          do
            check_file $file
          done
        sleep 5
done
