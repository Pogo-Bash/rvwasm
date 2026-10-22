#!/bin/bash
# In the VM, once: a Mesa 25.0.7 checkout for vfront (shares objects with the spikes' clone), with
# mesa-vfront.diff applied and vfront linked in.
set -e
V=$HOME/rvwasm/gpu-t6/vfront; M=$HOME/rvwasm/gpu-t6/mesa
if [ ! -d $M ]; then
	git clone -q --shared $HOME/rvwasm/webgpu/mesa-25.0.7 $M
	git -C $M checkout -q 742a20f48c59e8649533c84c4d49dd95b403f5da
fi
if ! git -C $M diff --quiet; then
	echo "mesa already has changes; not applying mesa-vfront.diff"
else
	git -C $M apply $V/mesa-vfront.diff
fi
ln -sfn $V $M/src/gallium/frontends/vfront
echo ok
