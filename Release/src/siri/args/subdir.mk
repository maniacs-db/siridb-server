################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/siri/args/args.c 

OBJS += \
./src/siri/args/args.o 

C_DEPS += \
./src/siri/args/args.d 


# Each subdirectory must supply rules for building sources it contributes
src/siri/args/%.o: ../src/siri/args/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -I../include -O3 -Wall $(CFLAGS) -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<" $(LDFLAGS)
	@echo 'Finished building: $<'
	@echo ' '


