#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)

ULP_APP_NAME ?= ulp_$(COMPONENT_NAME)
ULP_S_SOURCES = $(addprefix $(COMPONENT_PATH)/ulp/, dht11_sensor.S)
ULP_EXP_DEP_OBJECTS := sensor_deepsleep.o

include $(IDF_PATH)/components/ulp/component_ulp_common.mk
