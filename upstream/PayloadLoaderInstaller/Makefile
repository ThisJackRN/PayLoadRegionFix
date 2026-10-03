#-------------------------------------------------------------------------------
.SUFFIXES:
#-------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

#-------------------------------------------------------------------------------
# APP_NAME sets the long name of the application
# APP_SHORTNAME sets the short name of the application
# APP_AUTHOR sets the author of the application
#-------------------------------------------------------------------------------
APP_NAME	:= Payload-Loader Installer - RegionFix
APP_SHORTNAME	:= PLI RegionFix
APP_AUTHOR	:=  Maschell, rw-r-r-0644, GaryOderNichts

include $(DEVKITPRO)/wut/share/wut_rules

#-------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# CONTENT is the path to the bundled folder that will be mounted as /vol/content/
# ICON is the game icon, leave blank to use default rule
# TV_SPLASH is the image displayed during bootup on the TV, leave blank to use default rule
# DRC_SPLASH is the image displayed during bootup on the DRC, leave blank to use default rule
#-------------------------------------------------------------------------------
TARGET		:=	PayloadLoaderInstaller-RegionFix
BUILD		:=	build
DIAGNOSTIC_ONLY ?= 0
ifeq ($(DIAGNOSTIC_ONLY),1)
APP_NAME := PLI RegionFix Diagnostic
APP_SHORTNAME := PLI Diagnostic
TARGET := PayloadLoaderInstaller-RegionFix-Diagnostic
BUILD := build-diagnostic
endif
SOURCES		:=	source \
                source/common \
                source/utils \
                source/fs \
                source/input
INCLUDES	:=	include source
ICON		:=  meta/icon.png

#-------------------------------------------------------------------------------
# options for code generation
#-------------------------------------------------------------------------------
CFLAGS	:=	-g -Wall -O2 -ffunction-sections -fstack-protector-all \
			$(MACHDEP)

CFLAGS	+=	$(INCLUDE) -D__WIIU__ -D__WUT__
ifeq ($(DIAGNOSTIC_ONLY),1)
CFLAGS += -DREGIONFIX_DIAGNOSTIC_ONLY=1
endif

CXXFLAGS	:= $(CFLAGS) -std=c++17 

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-g $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lwut -liosuhax

#-------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level
# containing include and lib
#-------------------------------------------------------------------------------
LIBIOSUHAX ?= $(TOPDIR)/../libiosuhax
LIBDIRS	:= $(PORTLIBS) $(WUT_ROOT) $(WUT_ROOT)/usr $(LIBIOSUHAX)

# The local build reuses the exact payload from official installer v0.1.1.
# main.cpp still checks its original SHA1 at compile time.
USE_PREBUILT_PAYLOAD ?= 0


#-------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
ifeq ($(DIAGNOSTIC_ONLY),1)
# Do not even compile the install/restore/coldboot state machine for this app.
CPPFILES := $(filter-out ApplicationState.cpp,$(CPPFILES))
else
CPPFILES := $(filter-out DiagnosticState.cpp,$(CPPFILES))
endif
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

#-------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#-------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#-------------------------------------------------------------------------------
	export LD	:=	$(CC)
#-------------------------------------------------------------------------------
else
#-------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#-------------------------------------------------------------------------------
endif
#-------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)  root.rpx.o
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifneq (,$(strip $(CONTENT)))
	export APP_CONTENT := $(TOPDIR)/$(CONTENT)
endif

ifneq (,$(strip $(ICON)))
	export APP_ICON := $(TOPDIR)/$(ICON)
else ifneq (,$(wildcard $(TOPDIR)/$(TARGET).png))
	export APP_ICON := $(TOPDIR)/$(TARGET).png
else ifneq (,$(wildcard $(TOPDIR)/icon.png))
	export APP_ICON := $(TOPDIR)/icon.png
endif

ifneq (,$(strip $(TV_SPLASH)))
	export APP_TV_SPLASH := $(TOPDIR)/$(TV_SPLASH)
else ifneq (,$(wildcard $(TOPDIR)/tv-splash.png))
	export APP_TV_SPLASH := $(TOPDIR)/tv-splash.png
else ifneq (,$(wildcard $(TOPDIR)/splash.png))
	export APP_TV_SPLASH := $(TOPDIR)/splash.png
endif

ifneq (,$(strip $(DRC_SPLASH)))
	export APP_DRC_SPLASH := $(TOPDIR)/$(DRC_SPLASH)
else ifneq (,$(wildcard $(TOPDIR)/drc-splash.png))
	export APP_DRC_SPLASH := $(TOPDIR)/drc-splash.png
else ifneq (,$(wildcard $(TOPDIR)/splash.png))
	export APP_DRC_SPLASH := $(TOPDIR)/splash.png
endif

.PHONY: $(BUILD) clean all

#-------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
ifeq ($(USE_PREBUILT_PAYLOAD),1)
	@test -f payload/root.rpx
else
	@$(MAKE) --no-print-directory -C payload
endif
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#-------------------------------------------------------------------------------
clean:
	@echo clean ...
	make clean -C payload
	@rm -fr $(BUILD) $(TARGET).wuhb $(TARGET).rpx $(TARGET).elf

#-------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#-------------------------------------------------------------------------------
# main targets
#-------------------------------------------------------------------------------
#-------------------------------------------------------------------------------
# main targets
#-------------------------------------------------------------------------------

safe_payload := ../payload/root.rpx

all	:	$(OUTPUT).wuhb

$(safe_payload): 
	make -C ../payload
    
$(OUTPUT).wuhb	:	$(OUTPUT).rpx
$(OUTPUT).rpx	:	$(OUTPUT).elf
	@$(OBJCOPY) --remove-section=.note.GNU-stack $< $(OUTPUT).rpx-input.elf
	@elf2rpl $(OUTPUT).rpx-input.elf $@
	@echo built ... $(notdir $@)
$(OUTPUT).elf	:	$(OFILES) $(LIBIOSUHAX)/lib/libiosuhax.a
$(OFILES)       :   safe_payload.h

$(OFILES_SRC)	: $(HFILES_BIN)

#-------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#-------------------------------------------------------------------------------
safe_payload.h: $(safe_payload)
	@bin2s -a 32 -H `(echo $(<F) | tr . _)`.h $< | $(AS) -o $(<F).o
	@echo '#pragma once'  > $@
	@printf '#include "' >> $@
	@(printf $(<F) | tr . _) >> $@
	@echo '.h"' >> $@
	@printf '#define RPX_HASH "' >> $@ 
	@sha1sum $(<) |  cut -f1 -d' ' | tr -d '\n' >> $@
	@printf '"' >> $@ 

#-------------------------------------------------------------------------------
endif
#-------------------------------------------------------------------------------
