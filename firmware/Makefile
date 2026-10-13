TARGET   = kvm_switch
BUILD    = build

# Toolchain: arm-none-eabi-* on PATH, or point GCC_PATH at a bin directory,
# e.g. the one bundled with STM32CubeIDE (see README).
GCC_PATH ?=
PREFIX   = $(if $(GCC_PATH),$(GCC_PATH)/,)arm-none-eabi-
CC       = $(PREFIX)gcc
OBJCOPY  = $(PREFIX)objcopy
SIZE     = $(PREFIX)size

CMSIS    = Drivers/CMSIS
DEVICE   = $(CMSIS)/Device/ST/STM32C0xx

SRCS     = $(wildcard src/*.c) $(DEVICE)/Source/system_stm32c0xx.c
ASRCS    = $(DEVICE)/Source/startup_stm32c031xx.s
OBJS     = $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o) $(ASRCS:.s=.o)))
vpath %.c $(sort $(dir $(SRCS)))
vpath %.s $(sort $(dir $(ASRCS)))

MCU      = -mcpu=cortex-m0plus -mthumb
CFLAGS   = $(MCU) -std=c11 -Os -g3 -Wall -Wextra -Wshadow \
           -ffunction-sections -fdata-sections \
           -DSTM32C031xx -Isrc -I$(CMSIS)/Include -I$(DEVICE)/Include \
           -MMD -MP
LDFLAGS  = $(MCU) -TSTM32C031G6.ld --specs=nano.specs -nostartfiles \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map -Wl,--print-memory-usage

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(MCU) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) STM32C031G6.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/%.hex: $(BUILD)/%.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD):
	mkdir $@

# Flash with STM32CubeProgrammer CLI over ST-Link (SWD test points).
PROGRAMMER ?= STM32_Programmer_CLI
flash: $(BUILD)/$(TARGET).hex
	$(PROGRAMMER) -c port=SWD mode=UR -w $< -v -rst

clean:
	rm -rf $(BUILD)

.PHONY: all flash clean
-include $(OBJS:.o=.d)
