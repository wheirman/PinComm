PIN_KIT=/path/to/pin

##
## PIN tools
##

##############################################################
#
# Here are some things you might want to configure
#
##############################################################

TARGET_COMPILER?=gnu
ifdef OS
    ifeq (${OS},Windows_NT)
        TARGET_COMPILER=ms
    endif
endif

##############################################################
#
# include *.config files
#
##############################################################

ifeq ($(TARGET_COMPILER),gnu)
    include ${PIN_KIT}/source/tools/makefile.gnu.config
    LINKER?=${CXX}
    CXXFLAGS ?= -g -Wall -Werror -Wno-unknown-pragmas $(DBG) $(OPT)
endif

ifeq ($(TARGET_COMPILER),ms)
    include ${PIN_KIT}/source/tools/makefile.ms.config
    DBG?=
endif

##############################################################
#
# Tools sets
#
##############################################################


TOOL_ROOTS = pincomm
STATIC_TOOL_ROOTS = 

TOOLS = $(TOOL_ROOTS:%=$(OBJDIR)%$(PINTOOL_SUFFIX))
STATIC_TOOLS = $(STATIC_TOOL_ROOTS:%=$(OBJDIR)%$(SATOOL_SUFFIX))

##############################################################
#
# build rules
#
##############################################################

all: tools
tools: $(OBJDIR) $(TOOLS) $(STATIC_TOOLS)
test: $(OBJDIR) $(TOOL_ROOTS:%=%.test) $(STATIC_TOOL_ROOTS:%=%.test) 


# stand alone pin tool
statica.test: $(OBJDIR)statica$(SATOOL_SUFFIX) statica.tested statica.failed $(OBJDIR)statica
	./$(OBJDIR)statica$(SATOOL_SUFFIX) -i ./$(OBJDIR)statica  > statica.dmp
	rm statica.failed statica.dmp

replacesigprobed.test : $(OBJDIR)replacesigprobed$(PINTOOL_SUFFIX) replacesigprobed.tested replacesigprobed.failed
	$(PIN) -probe -t $< -- $(TESTAPP) makefile $<.makefile.copy >  $<.out 2>&1
	rm replacesigprobed.failed  $<.out $<.makefile.copy

## build rules

$(OBJDIR):
	mkdir -p $(OBJDIR)


$(OBJDIR)%.o : %.cpp binstore/libbinstore.a
	$(CXX) -c $(CXXFLAGS) $(PIN_CXXFLAGS) -Ibinstore ${OUTOPT}$@ $<

$(TOOLS): $(PIN_LIBNAMES) binstore/libbinstore.a Makefile

$(TOOLS): %$(PINTOOL_SUFFIX) : %.o
	${LINKER} $(PIN_LDFLAGS) $(LINK_DEBUG) ${LINK_OUT}$@ $< ${PIN_LPATHS} $(PIN_LIBS) -Lbinstore -lbinstore -lz $(DBG)

$(STATIC_TOOLS): $(PIN_LIBNAMES)

$(STATIC_TOOLS): %$(SATOOL_SUFFIX) : %.o
	${LINKER} $(PIN_SALDFLAGS) $(LINK_DEBUG) ${LINK_OUT}$@ $< ${PIN_LPATHS} $(SAPIN_LIBS) -Lbinstore -lbinstore -lz $(DBG)

## cleaning
clean:
	-rm -rf $(OBJDIR) *.tested *.failed *.makefile.copy 


example: example.c
	gcc -g -O1 -o example example.c

demo: $(TOOLS) example
	-$(PIN_KIT)/pin -t $(OBJDIR)/pincomm.so -- ./example
	./pinprocess.py --groupby r
