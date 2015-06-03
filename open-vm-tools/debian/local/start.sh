#!/bin/bash

pid_file="../var/run/vmw.pid"

function start_vmtoolsd {
   echo "starting vmtoolsd chrooted"
   chroot ../../vmw /bin/vmtoolsd &
   mypid=$!
   #add the PID to the log file
   echo $mypid > $pid_file
}


#killall check_sys_files.sh 2> /dev/null
#./check_sys_files.sh > /dev/null &
#check if vmtoolsd is already running
if [ -e $pid_file ]
then
#get registered pid and current pids list
   vmw_pid=$(cat $pid_file)
   myarr=($(ps haux | awk '{ print $2 }'))

# loop through every element in the array
   for i in "${myarr[@]}"
   do
      :
      if [ $i -eq $vmw_pid ]
        then
           echo "vmtoolsd is already running"
           exit 0
        fi
   done
  rm -rf $pid_file
  start_vmtoolsd
else
  start_vmtoolsd
fi

