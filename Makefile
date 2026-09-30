# ======================== 1. 交叉编译器 ========================
# COMPILE_PREX 由 build.sh 传入，指向 ARM musl 工具链前缀。
# .c 文件用 gcc 编译，.cpp 和最终可执行文件用 g++ 处理，
# 因为 RTSP TinyServer 是 C++ 库，最终链接时需要 C++ 运行库。
CXX = $(COMPILE_PREX)
AR = $(COMPILE_PREX)ar
CC = $(COMPILE_PREX)gcc
XX = $(CXX)g++
NM = $(COMPILE_PREX)nm
CPP = $(COMPILE_PREX)g++
STRIP = $(COMPILE_PREX)strip
OBJCOPY = $(COMPILE_PREX)objcopy
OBJDUMP = $(COMPILE_PREX)objdump

# 让复制出来的 OpenWrt 工具链不再依赖 Tina SDK 原有的 STAGING_DIR。
TOOLCHAIN_ROOT := $(abspath $(dir $(COMPILE_PREX))/..)
# The relocated GCC toolchain no longer has Tina's original staging sysroot.
# Put its C headers after GCC/libstdc++ headers so C++ #include_next directives
# (for example <cstdlib> -> <stdlib.h>) can resolve them correctly.
TOOLCHAIN_SYS_INCS := -idirafter $(TOOLCHAIN_ROOT)/include
TOOLCHAIN_SYS_LIBS := -B$(TOOLCHAIN_ROOT)/lib/ -L$(TOOLCHAIN_ROOT)/lib

# V853 芯片宏，SDK 头文件会根据它选择对应实现。
AWCHIP_CFLAGS := -DAWCHIP=0x1886#AW_V853

# ======================== 2. 项目路径 ========================
# abspath 把相对路径转成绝对路径，避免从不同目录调用 make 时找错文件。
ROOT_DIR = $(abspath .)
OUTPUT_DIR = $(ROOT_DIR)/output/
OUTPUT_DIR_OBJS = $(OUTPUT_DIR)/objs

#sammple目录
USER_SRC_BASE_DIR 	=  $(ROOT_DIR)/sample

# ======================== 3. SDK 库目录 ========================
# MPP、ISP、NPU 等库都保存在项目 sdk/ 内，因此编译不依赖父目录。
mpp_lib_dir=$(ROOT_DIR)/sdk/aw_pack_src/lib_aw/lib/eyesee-mpp

aiisp_dir=$(ROOT_DIR)/sdk/aw_pack_src/lib_aw/lib/libawaiisp

viplite_dir=$(ROOT_DIR)/sdk/aw_pack_src/lib_aw/lib/viplite-driver

awnnfull_dir=$(ROOT_DIR)/sdk/aw_pack_src/lib_aw/lib/libawnn_full

#需要使用的动态库目录
rootfs_share_lib_dir=$(ROOT_DIR)/sdk/share_lib

#包含多媒体库位置
LINKFLAGS = \
	$(TOOLCHAIN_SYS_LIBS) \
	-lpthread -lrt -ldl -lm -Wl,--gc-sections \
	-L$(mpp_lib_dir) \
	-L$(rootfs_share_lib_dir) \
	-L$(aiisp_dir) \
	-L$(viplite_dir) -lpix_facekit_api
#	-L$(awnnfull_dir)

#自动化链接库文件夹里面所有的.a库
#遍历所有的.a库文件
lib_files := $(foreach dir,$(mpp_lib_dir),$(wildcard $(dir)/*.a))
#将文件去掉目录，只保留文件名
lib_nordir :=  $(notdir $(lib_files))
#将库的名字去除头lib 和 尾的.a
lib_name := $(patsubst lib%,%,$(patsubst %.a,%,$(lib_nordir)))
#给所有库添加-l参数
ld_mpp_libs := $(addprefix -l, $(lib_name))

# 处理 aiisp_dir 中的 .a 库
lib_files_aiisp := $(foreach dir,$(aiisp_dir),$(wildcard $(dir)/*.a))
lib_nordir_aiisp :=  $(notdir $(lib_files_aiisp))
lib_name_aiisp := $(patsubst lib%,%,$(patsubst %.a,%,$(lib_nordir_aiisp)))
ld_aiisp_libs := $(addprefix -l, $(lib_name_aiisp))

 # 处理 viplite_dir 中的 .a 库
lib_files_viplite := $(foreach dir,$(viplite_dir),$(wildcard $(dir)/*.a))
lib_nordir_viplite :=  $(notdir $(lib_files_viplite))
lib_name_viplite := $(patsubst lib%,%,$(patsubst %.a,%,$(lib_nordir_viplite)))
ld_viplite_libs := $(addprefix -l, $(lib_name_viplite))

 # 处理 awnnfull_dir 中的 .a 库
lib_files_awnnfull := $(foreach dir,$(awnnfull_dir),$(wildcard $(dir)/*.a))
lib_nordir_awnnfull :=  $(notdir $(lib_files_awnnfull))
lib_name_awnnfull := $(patsubst lib%,%,$(patsubst %.a,%,$(lib_nordir_awnnfull)))
ld_awnnfull_libs := $(addprefix -l, $(lib_name_awnnfull))

mpp_lib := \
  -Wl,-Bstatic -Wl,--whole-archive -Wl,--start-group \
  $(ld_mpp_libs) $(ld_aiisp_libs) $(ld_viplite_libs) \
  -Wl,--end-group \
  -Wl,--no-whole-archive \
  -Wl,-Bdynamic  \
  -lglog \
  -lasound \
  -lion \
  -law_mpp \
  -lmedia_utils \
  -liniparser \
  -lMemAdapter \
  -lVE \
  -lcdc_base \
  -lcdx_common \
  -lawaiisp \
  -lVIPlite \
  -lawnn \
  -lVIPuser \
  -lpix_facekit_api \
  -lrgb_ctrl
#  -lawion 

#手动多媒体包含的库，根据自己的选择增减
#mpp_lib := \
#  -Wl,-Bstatic -Wl,--whole-archive -Wl,--start-group \
#  -llog \
#  -lion  \
#  -law_mpp \
#  -lmedia_utils \
#  -lMemAdapter  \
#  -lVE  \
#  -lcdc_base  \
#  -lcedarxstream  \
#  -lsample_confparser  \
#  -lcdx_common  \
#  -lcdx_base  \
#  -lResample  \
#  -lISP  \
#  -lisp_dev  \
#  -lisp_ini  \
#  -liniparser  \
#  -lisp_ae  \
#  -lisp_af  \
#  -lisp_afs  \
#  -lisp_awb  \
#  -lisp_base  \
#  -lisp_gtm  \
#  -lisp_iso  \
#  -lisp_math  \
#  -lisp_md  \
#  -lisp_pltm  \
#  -lisp_rolloff \
#  -lvencoder  \
#  -lvenc_codec  \
#  -lvenc_base  \
#  -lcedarx_aencoder  \
#  -lAec \
#  -lDrc  \
#  -lAgc  \
#  -lAns  \
#  -lVIPlite \
#  -lVIPuser \
#  -lawnn_det \
#  -Wl,--end-group \
#  -Wl,--no-whole-archive \
#  -Wl,-Bdynamic  \
#  -lglog \
#  -lasound

OPENCV_LIBS = \
	-lopencv_objdetect \
	-lopencv_calib3d \
	-lopencv_features2d \
	-lopencv_highgui \
	-lopencv_videoio \
	-lopencv_imgcodecs \
	-lopencv_video \
	-lopencv_photo \
	-lopencv_ml \
	-lopencv_imgproc \
	-lopencv_flann \
	-lopencv_core \
	-lwebp \
	-lpng \
	-ljpeg

COMMON_FLAGS = -g
CXX_ONLY_FLAGS = -std=c++11 -fpermissive

#加一些编译参数。其中-Wl,-z,stack-size=1048576 表示设置线程堆栈为1M,避免不够用导致崩溃
DEFINES = \
	-Os -pipe -march=armv7-a -mtune=cortex-a7 -mfpu=neon -g3 -fno-caller-saves -Wno-unused-result -mfloat-abi=hard  -Wformat  -Werror=format-security -fstack-protector -Wl,-z,now -Wl,-z,relro -Wl,-z,stack-size=1048576 

#全志头文件路径
#USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/viplite
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/viplite-driver
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/include/utils
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/include/media
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/include
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/include
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/include/component
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/libisp/include
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/libisp/isp_dev
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/libisp/include/V4l2Camera
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/libisp/isp_tuning
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/include_stream
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/include_FsWriter
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/libcedarc/include
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/include_muxer
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/libResample/include
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/libcedarx/libcore/common/iniparser
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/sample/configfileparser/
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/agc_float_lib/include
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/system/public/include
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/libawlist
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/LIBRARY/libisp
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/middleware/media/utils
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/libawaiisp
#USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/libawaiisp/viplite-driver
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/system/public/libion/include
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/system/public/rgb_ctrl/
USER_INC_BASE_DIR  += $(ROOT_DIR)/sdk/aw_pack_src/lib_aw/include/eyesee-mpp/system/private/rtsp/IPCProgram/interface

#user app 编译文件
USER_SRC_DIRS = $(shell find $(USER_SRC_BASE_DIR) -type d)
USER_SRCS += $(foreach dir, $(USER_SRC_DIRS), $(wildcard $(dir)/*.c)) 
USER_SRCS += $(foreach dir, $(USER_SRC_DIRS), $(wildcard $(dir)/*.cpp)) 
USER_SRCS += $(foreach dir, $(USER_SRC_DIRS), $(wildcard $(dir)/*.s)) 
USER_SRCS += $(foreach dir, $(USER_SRC_DIRS), $(wildcard $(dir)/*.S)) 
#user头文件
USER_INCS  = -I$(ROOT_DIR)/sample/ipc_camera/include/
USER_INCS += -I$(ROOT_DIR)/sample/common/
USER_INCS += $(addprefix -I ,$(shell find $(USER_INC_BASE_DIR) -type d))
USER_INCS += -I$(ROOT_DIR)/sdk/share_include/opencv4/

#user的obj命令
USER_OBJS = $(addsuffix .o, $(basename  $(USER_SRCS) ) )
#user的实际obj地址
USER_OBJS_OUT =  $(subst $(ROOT_DIR),$(OUTPUT_DIR_OBJS), $(USER_OBJS))

CFLAGS = $(COMMON_FLAGS) $(DEFINES) $(TOOLCHAIN_SYS_INCS) $(SDK_INCS) $(USER_INCS) $(AWCHIP_CFLAGS)
CXXFLAGS = $(COMMON_FLAGS) $(CXX_ONLY_FLAGS) $(DEFINES) $(TOOLCHAIN_SYS_INCS) $(SDK_INCS) $(USER_INCS) $(AWCHIP_CFLAGS)

build_app: $(USER_OBJS)
	@mkdir -p $(OUTPUT_DIR)
	$(XX)  $(USER_OBJS_OUT) $(CFLAGS)  $(LINKFLAGS) $(mpp_lib) $(OPENCV_LIBS) -o $(OUTPUT_DIR)/$(APP_BIN_NAME) 
	@echo "Build APP Finish"
	@echo Linking: $(USER_OBJS_OUT)
	@cp $(OUTPUT_DIR)/$(APP_BIN_NAME) $(OUTPUT_DIR)/$(APP_BIN_NAME)_strip
	$(STRIP) $(OUTPUT_DIR)/$(APP_BIN_NAME)_strip

%.o: %.c
	@mkdir -p $(dir $(subst $(ROOT_DIR),$(OUTPUT_DIR_OBJS), $@)); 
	$(CC) $(CFLAGS) -o  $(subst $(ROOT_DIR),$(OUTPUT_DIR_OBJS), $@)   -c $<

%.o: %.cpp
	@mkdir -p $(dir $(subst $(ROOT_DIR),$(OUTPUT_DIR_OBJS), $@)); 
	$(XX) $(CXXFLAGS) -o $(subst $(ROOT_DIR),$(OUTPUT_DIR_OBJS), $@)   -c $<

%.o: %.s
	@mkdir -p $(dir $(subst $(ROOT_DIR),$(OUTPUT_DIR_OBJS), $@)); 
	$(CC) $(CFLAGS) -o $(subst $(ROOT_DIR),$(OUTPUT_DIR_OBJS), $@)  -c $<

%.o: %.S
	@mkdir -p $(dir $(subst $(ROOT_DIR),$(OUTPUT_DIR_OBJS), $@)); 
	$(CC) $(CFLAGS) -D__ASSEMBLER__ -o $(subst $(ROOT_DIR),$(OUTPUT_DIR_OBJS), $@)  -c $<


.PHONY:clean SHOWARGS
clean:
	rm -rf $(OUTPUT_DIR)

