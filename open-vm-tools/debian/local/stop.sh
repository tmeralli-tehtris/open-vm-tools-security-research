#!/bin/bash

killall check_sys_files.sh 2> /dev/null
#check if vmtoolsd is already running
if [ -e ../var/vmw.pid ]
then
#get registered pid and current pids list
   vmw_pid=$(cat ../var/vmw.pid)
   myarr=($(ps haux | awk '{ print $2 }'))

# loop through every element in the array
   for i in "${myarr[@]}"
   do
      :
      if [ $i -eq $vmw_pid ]
        then
           echo "killing vmtoolsd"
	   kill -9 $vmw_pid
           exit 0
        fi
   done
  echo "vmtoolsd was not running"
  rm -rf ../var/vmw.pid
else
   killall -9 vmtoolsd 2> /dev/null
fi

