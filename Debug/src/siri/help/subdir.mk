################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/siri/help/help.c 

OBJS += \
./src/siri/help/help.o 

C_DEPS += \
./src/siri/help/help.d 


# Each subdirectory must supply rules for building sources it contributes
src/siri/help/%.o: ../src/siri/help/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -DDEBUG=1 -I../include -O0 -g3 -Wall $(CFLAGS) -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<" $(LDFLAGS)
	@echo 'Finished building: $<'
	@echo ' '


