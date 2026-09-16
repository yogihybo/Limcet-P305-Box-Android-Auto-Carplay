# Live BD37033 Validation TCL script
gdb_port disabled
tcl_port disabled
telnet_port disabled

init

echo "======================================================="
echo " LIVE ROHM BD37033 STATE MACHINE & I2C SILICON TEST"
echo "======================================================="

# Reset target to bootloader and let it run into app
reset halt
mww 0xE0042004 0x00000307
resume

echo "\n--- Point A: t = 1.0s after cold boot ---"
sleep 1000
halt
mww 0xE0042004 0x00000307

set pc_a [reg pc]
set soundtype [mrb 0x200001F2]
set audio_step_a [mrb 0x2000102D]
set audio_timer_a [mrh 0x2000007E]
set pwr_timer_a [mrh 0x20000052]
set pwr_state_a [mrb 0x200007E8]

echo "PC: $pc_a"
echo "SoundType (*0x200001F2): [format 0x%02X $soundtype]"
echo "Audio Step (*0x2000102D): [format 0x%02X $audio_step_a]"
echo "Audio Timer (*0x2000007E): $audio_timer_a"
echo "Power Timer (*0x20000052): $pwr_timer_a"
echo "Power State (*0x200007E8): [format 0x%02X $pwr_state_a]"

# Resume execution and wait past the 4.0-second delay
echo "\n--- Running for 3.5s to cross 4.0s settling threshold ---"
resume
sleep 3500
halt
mww 0xE0042004 0x00000307

echo "\n--- Point B: t = 4.5s after cold boot ---"
set pc_b [reg pc]
set audio_step_b [mrb 0x2000102D]
set audio_timer_b [mrh 0x2000007E]
set pwr_timer_b [mrh 0x20000052]
set pwr_state_b [mrb 0x200007E8]
set i2c_err [mrb 0x20000094]

set i2c1_cr1 [mrw 0x40005400]
set i2c1_cr2 [mrw 0x40005404]
set i2c1_sr1 [mrw 0x40005414]
set i2c1_sr2 [mrw 0x40005418]

set gpiob_crl [mrw 0x40010C00]
set gpiob_idr [mrw 0x40010C08]

echo "PC: $pc_b"
echo "Audio Step (*0x2000102D): [format 0x%02X $audio_step_b]"
echo "Audio Timer (*0x2000007E): $audio_timer_b"
echo "Power Timer (*0x20000052): $pwr_timer_b"
echo "Power State (*0x200007E8): [format 0x%02X $pwr_state_b]"
echo "I2C Error Count (*0x20000094): [format 0x%02X $i2c_err]"
echo "I2C1->CR1: [format 0x%08X $i2c1_cr1]"
echo "I2C1->CR2: [format 0x%08X $i2c1_cr2]"
echo "I2C1->SR1: [format 0x%08X $i2c1_sr1]"
echo "I2C1->SR2: [format 0x%08X $i2c1_sr2]"
echo "GPIOB->CRL (PB6/PB7 config): [format 0x%08X $gpiob_crl]"
echo "GPIOB->IDR (PB6=SCL, PB7=SDA): [format 0x%08X $gpiob_idr]"

# Dump audio volume / fader RAM block at 0x20001016
echo "\nAudio Config Block (*0x20001014..0x20001034):"
mdw 0x20001014 8

echo "\n======================================================="
resume
exit
