################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/qpack/qpack.c 

OBJS += \
./src/qpack/qpack.o 

C_DEPS += \
./src/qpack/qpack.d 


# Each subdirectory must supply rules for building sources it contributes
src/qpack/%.o: ../src/qpack/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -DDEBUG=1 -I"/home/joente/workspace/siridb-server/include" -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

