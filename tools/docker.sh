#!/usr/bin/env bash

# © Copyright 2025 Xilinx, Inc. All rights reserved.
# This file contains confidential and proprietary information of Xilinx, Inc. and is protected under U.S. and
# international copyright and other intellectual property laws.
#
# DISCLAIMER
# This disclaimer is not a license and does not grant any rights to the materials distributed herewith.
# Except as otherwise provided in a valid license issued to you by Xilinx, and to the maximum extent
# permitted by applicable law: (1) THESE MATERIALS ARE MADE AVAILABLE "AS IS" AND WITH ALL
# FAULTS, AND XILINX HEREBY DISCLAIMS ALL WARRANTIES AND CONDITIONS, EXPRESS, IMPLIED, OR
# STATUTORY, INCLUDING BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY, NONINFRINGEMENT, OR FITNESS FOR ANY PARTICULAR PURPOSE; and (2) Xilinx shall not be liable (whether
# in contract or tort, including negligence, or under any other theory of liability) for any loss or damage of
# any kind or nature related to, arising under or in connection with these materials, including for any
# direct, or any indirect, special, incidental, or consequential loss or damage (including loss of data,
# profits, goodwill, or any type of loss or damage suffered as a result of any action brought by a third
# party) even if such damage or loss was reasonably foreseeable or Xilinx had been advised of the
# possibility of the same.
#
# CRITICAL APPLICATIONS
# Xilinx products are not designed or intended to be fail-safe, or for use in any application requiring failsafe performance, such as life-support or safety devices or systems, Class III medical devices, nuclear
# facilities, applications related to the deployment of airbags, or any other applications that could lead to
# death, personal injury, or severe property or environmental damage (individually and collectively,
# "Critical Applications"). Customer assumes the sole risk and liability of any use of Xilinx products in
# Critical Applications, subject only to applicable laws and regulations governing limitations on product
# liability.
#
# THIS COPYRIGHT NOTICE AND DISCLAIMER MUST BE RETAINED AS PART OF THIS FILE AT ALL TIMES


EXTRA_OPTS="$*"

if [ -z "$HOST_REPO" ]; then
  HOST_REPO=$(realpath $( ( cd $(dirname $0) && git rev-parse --show-toplevel ) )/..)
fi
PWD=$(realpath $PWD)

DOCKER_RUN_AS=true
if [ -z "$DOCKER_RUN_AS_COMMAND" ] && [[ $PWD == ${HOST_REPO}* ]]; then
  DOCKER_EXEC_DIR="~/ryzen-ai/${PWD#${HOST_REPO}}"
  DOCKER_RUN_AS_COMMAND="cd \"${DOCKER_EXEC_DIR}\" && /bin/bash -l"
fi

for i in $(seq 0 15); do
  [ -c /dev/accel/accel$i ] && echo "Adding device /dev/accel/accel$i" && DOCKER_EXTRA+=" --device /dev/accel/accel$i"
done

test_for_keys=$(stat -t ~/.ssh/id* > /dev/null 2>&1)
if [ $? = 0 ]; then
    TDIR=$(mktemp -d) && cp -R ~/.ssh/* $TDIR && DOCKER_DATA_MOUNTS+=",$TDIR:.ssh" && trap 'rm -rf -- "$TDIR"' EXIT # TODO worry about cleanup
else
  echo "ssh keys not found, be careful when cloning github repositories"
fi
[ -f ~/.gitconfig ] && cp ~/.gitconfig /tmp/.gitconfig-$USER && DOCKER_DATA_MOUNTS+=",/tmp/.gitconfig-$USER:.gitconfig"

HOME=/proj/rdi/staff/$USER

DOCKER_EXTRA+=" --security-opt seccomp=unconfined -v /var/run/docker.sock:/var/run/docker.sock"

[ -z "$DOCKER_NAME" ] && DOCKER_NAME=ipu_fw_dev_ubuntu
[ -z "$DOCKER_IMAGE" ] && DOCKER_IMAGE=registry.amd.com/ipufw/ipu_fw_dev_ubuntu

for D in /proj/xbuilds /opt; do
  [ -d "$D" ] && DOCKER_DATA_MOUNTS+=,${D}:${D}
done
DOCKER_DATA_MOUNTS+=",/proj/xbuilds/SWIP/2024.2.2_0306_2141/installs/lin64/Vitis/2024.2/:/mnt/packages/vitis"

[ -d /mnt/repos ] && echo "Mounting /mnt/repos" && DOCKER_EXTRA+=" -v /mnt/repos:/mnt/repos"

XAUTHORITY=/tmp/.Xauthority-$USER
[ -z "$DOCKER_DISPLAY" ] && DOCKER_DISPLAY="${DISPLAY-unix:0.0}"
DOCKER_ARGS+=" -v /tmp/.X11-unix:/tmp/.X11-unix -e DISPLAY=$DOCKER_DISPLAY"
DOCKER_ARGS+=" -v ${XAUTHORITY}:/tmp/xauthority:ro -e XAUTHORITY=/tmp/xauthority"
[ -S "$SSH_AUTH_SOCK" ] && DOCKER_ARGS+=" -v $(dirname $SSH_AUTH_SOCK):$(dirname $SSH_AUTH_SOCK) -e SSH_AUTH_SOCK=$SSH_AUTH_SOCK"

[ -z "$DOCKER_NOT_TRANSIENT" ] && DOCKER_ARGS+=" --rm"
[ ! -z "$DOCKER_PORTS" ] && DOCKER_ARGS+=" $DOCKER_PORTS"
[ ! -z "$DOCKER_EXTRA" ] && DOCKER_ARGS+=" $DOCKER_EXTRA"
[ -z "$DOCKER_DISABLE_INTERACTIVE" ] && DOCKER_TERM_ARGS+=" -i"
[ -z "$DOCKER_DISABLE_TTY" ] && DOCKER_TERM_ARGS+=" -t"

docker login registry.amd.com --username $USER
if [ $? -ne 0 ]; then
  echo "ERROR: Unable to login to docker registry"
  exit 1
fi
docker pull ${DOCKER_IMAGE}
if [ $? -ne 0 ]; then
  echo "ERROR: Unable to pull docker image ${DOCKER_IMAGE}"
  exit 1
fi

DOCKER_DATA_MOUNTS+=",${HOST_REPO}:ryzen-ai"
[ -f ${HOME}/${DOCKER_NAME}-bashrc ] && DOCKER_DATA_MOUNTS+=",/${HOME}/${DOCKER_NAME}-bashrc:/tmp/.bashrc"

DOCKER_ARGS+=" -e THIS_DOCKER_IMAGE=$DOCKER_IMAGE"
DOCKER_ARGS+=" --name $DOCKER_NAME-$USER"

if [ ! -z "$DOCKER_DATA_MOUNTS" ]
then
  for MOUNT in $(echo $DOCKER_DATA_MOUNTS|tr ' ,' '~ ')
  do
    set $(echo $MOUNT|tr ':!' ' :')
    if [ "${1:0:1}" == "." ]
    then
      hostdir="$PWD/"
    else
      hostdir=""
    fi
    hostdir+="$(echo $1|tr '~' ' ')"
    if [ ! -d "$hostdir" -a ! -f "$hostdir" ]
    then
      echo "Cannot locate file or directory on host: $hostdir" >&2
      exit 1
    fi
    DOCKER_ARGS+=" -v $hostdir:"
    if [ "${2:0:1}" != "/" ]
    then
      { ! [ -z "$DOCKER_RUN_AS" ] && DOCKER_ARGS+="/home/mapped/"; } || DOCKER_ARGS+="/home/user/"
    fi
    DOCKER_ARGS+=$(echo $2|tr '~' ' '):shared
  done
fi

DOCKER_ARGS+=" --hostname ipu_fw_dev --mac-address=60-45-cb-a1-4f-4e"

if [ ! -z "$DOCKER_NAME" ] && docker inspect --type container $DOCKER_NAME-$USER >/dev/null 2>&1
then
  [ -z "$EXTRA_OPTS" ] && EXTRA_OPTS="/bin/bash -il" &&  echo "***** Container already running, attaching *****"
  ! [ -z "$DOCKER_RUN_AS" ] && DOCKER_USER=$(id -nu)
  exec docker exec -e DOCKER_EXEC=true -e TERM=${TERM-dumb} -u ${DOCKER_USER-user} ${DOCKER_TERM_ARGS} $DOCKER_NAME-$USER bash -ci 'export USER=$(whoami)'" && cd ${DOCKER_EXEC_DIR} && $EXTRA_OPTS"
else
  ! [ -z "$EXTRA_OPTS" ] && echo "ERROR: Command given and container not running" >&2 && exit 1
  CURDIR="$(printf "%q\n" "$(pwd)")"  # Handle directories with spaces
  DOCKER_ARGS+=" -v $(realpath $CURDIR):/curdir"

  [ -z "$DOCKER_RUN_AS_COMMAND" ] && DOCKER_RUN_AS_COMMAND="/bin/bash -l"
  RUNAS_COMMAND="sudo -EH bash /tmp/remapper $(id -u) $(id -un) $(id -g) $(id -gn) -g ngcodec,docker \"${DOCKER_RUN_AS_COMMAND}\""
  docker run --init $DOCKER_TERM_ARGS $DOCKER_ARGS $DOCKER_IMAGE bash -ci "$RUNAS_COMMAND"
fi
