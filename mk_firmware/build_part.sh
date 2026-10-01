#!/bin/bash

Debug=0

#get build_part.sh dir
cd `dirname $0`
build_part_dir=`pwd`
cd -


tools_dir=${build_part_dir}/tools
mkfs_jffs2_tool=${tools_dir}/mkfs.jffs2

aw_fw_pack_dir=${build_part_dir}/../sdk/aw_pack_src
aw_fw_pack_image_dir=${aw_fw_pack_dir}/image

sdk_dir=${build_part_dir}/../
sdk_out_dir=${sdk_dir}/output
sdk_tmp_dir=${sdk_out_dir}/fex

app_name=
rootfs_image_name=rootfs.fex
aw_image_name=tina_ipc_uart0.img

#####################squashfs configuare####################
SQUASHFS_COMP=xz
#SQUASHFS_COMP=gzip
SQUASHFS_BLOCKSIZE=256k
SQUASHFSOPT="-b ${SQUASHFS_BLOCKSIZE}"
############################################################

make_new_rootfs()
{
	tmp_rootfs=${build_part_dir}/rootfs-tmp

	rm -rf ${tmp_rootfs}
	mkdir -p ${tmp_rootfs}
	tar -xzvf ${aw_fw_pack_dir}/rootfs/rootfs.tar.gz -C ${tmp_rootfs}
	if [ $? != 0 ]; then
		echo "decompression rootfs.tar.gz fail"
		return 2
	fi

	#cp app
	rm -f ${tmp_rootfs}/bin/${app_name} ${tmp_rootfs}/usr/bin/${app_name}
	cp -rf ${sdk_out_dir}/${app_name}_strip ${tmp_rootfs}/usr/bin/${app_name} -v

	# 阶段9：把随项目保存的YOLOv8 NPU模型安装到程序默认查找位置。
	if [ ! -f ${sdk_dir}/models/yolov8n.nb ]; then
		echo "not find ${sdk_dir}/models/yolov8n.nb"
		return 2
	fi
	cp -f ${sdk_dir}/models/yolov8n.nb ${tmp_rootfs}/lib/yolov8n.nb -v

	mkdir -p $sdk_tmp_dir

	#make rootfs image
	${tools_dir}/mksquashfs4 ${tmp_rootfs} ${sdk_tmp_dir}/${rootfs_image_name} -noappend -root-owned -comp ${SQUASHFS_COMP} ${SQUASHFSOPT}
	if [ $? != 0 ]; then
		echo "make rootfs squashfs fail"
		return 2
	fi

	if [ ${Debug} = 0 ]; then
	#rm tmp file and dir
		rm -rf ${tmp_rootfs}
	fi
}

make_aw_image(){

	echo "make AW image...."
	if [ ! -f ${aw_fw_pack_image_dir}/${rootfs_image_name} ]; then
		echo "not find ${rootfs_image_name} in aw_fw image"
		exit 1
	fi

	cp -f ${sdk_tmp_dir}/${rootfs_image_name} ${aw_fw_pack_image_dir}

	${aw_fw_pack_dir}/aw_pack.sh
	if [ $? != 0 ]; then
		echo "make aw image fail"
		exit 1
	fi

	cp ${aw_fw_pack_dir}/out/*.img ${sdk_out_dir}/${aw_image_name}
}


#set trsp app name
if [ -z $1 ]; then
	app_name=sample
else
	app_name=$1
fi

make_new_rootfs
if [ $? != 0 ]; then
	echo "make owm partition fail"
	exit 2
fi

#make AW image
make_aw_image



