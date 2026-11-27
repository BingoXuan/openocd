# SPDX-License-Identifier: GPL-2.0-or-later
# Host-only contract checks. Every target I/O command is a mock.
# Usage: jimsh flash-contracts.tcl <agrv2k-flash.tcl> <temporary-directory>

set helper [lindex $argv 0]
set temp [lindex $argv 1]
set AGRV2K_TARGET agrv2k.cpu
set AGRV2K_FLASH_BASE 0x80000000
set AGRV2K_FLASH_SIZE 0x100000
set AGRV2K_FLASH_WRITABLE_SIZE 0xe7000
set AGRV2K_FLASH_SECTOR_SIZE 0x1000
set AGRV2K_FLASH_CONTROLLER 0x40001000
set writes {}
set verified 0
set flash_calls {}
set mock_flash_error 0
set mock_time 0
set mock_state halted
set mock_fault 0
set mock_unlock_read_error 0
set key_state 0
set regs(1073745932) 0
set regs(1073745936) 128

proc assert_equal {actual expected message} {
	if {$actual ne $expected} { error "$message: got $actual, expected $expected" }
}
proc clock {operation} { global mock_time; return $mock_time }
proc sleep {duration} { global mock_time; incr mock_time $duration }
proc targets {name} {}
proc agrv2k.cpu {operation} { global mock_state; return $mock_state }
proc verify_image {filename address type} { global verified; incr verified }
proc flash {operation filename address type} {
	global flash_calls mock_flash_error
	if {$mock_flash_error} { error "injected native loader failure" }
	lappend flash_calls [list $operation $filename $address $type]
}
proc read_memory {address width count} {
	global regs mock_unlock_read_error key_state
	set address [expr {$address}]
	if {$mock_unlock_read_error && $key_state && $address == 0x40001010} {
		set mock_unlock_read_error 0
		error "injected control read failure after unlock"
	}
	if {![info exists regs($address)]} { return [list 0] }
	return [list $regs($address)]
}
proc mww {address value} {
	global regs writes key_state mock_fault
	set address [expr {$address}]
	set value [expr {$value}]
	lappend writes [list $address $value]
	if {$address == 0x40001004} {
		if {$value == 0x45670123} { set key_state 1 }
		if {$value == 0xcdef89ab && $key_state} {
			set regs(1073745936) [expr {$regs(1073745936) & ~128}]
		}
		return
	}
	if {$address == 0x4000100c} {
		set regs($address) [expr {$regs($address) & ~$value}]
		return
	}
	set regs($address) $value
	if {($address == 0x40001010 && ($value & 0x42) == 0x42) ||
		($address >= 0x80000000 && $address < 0x80100000)} {
		set regs(1073745932) [expr {$mock_fault == 1 ? 4 : ($mock_fault == 2 ? 1 : 32)}]
	}
}
source $helper

set rejected [catch {agrv2k_flash_erase 0x800e7000 4096}]
assert_equal $rejected 1 "CPLD range must be rejected"
assert_equal [llength $writes] 0 "Rejected range must not write registers"
assert_equal [catch {agrv2k_flash_erase 0x80000001 4096}] 1 "Unaligned erase rejected"
assert_equal [llength $writes] 0 "Alignment rejection must not write registers"
assert_equal [catch {agrv2k_flash_bounds 0x80000000 0x7fffffffffffffff}] 1 "Overflowing range rejected"
assert_equal [catch {agrv2k_flash_bounds 0x7fffffffffffffff 4096}] 1 "Overflowing address rejected"

set mock_state running
assert_equal [catch {agrv2k_flash_erase 0x800e5000 4096}] 1 "Running target rejected"
assert_equal [llength $writes] 0 "Running target must not unlock flash"
set mock_state halted

set mock_unlock_read_error 1
assert_equal [catch {agrv2k_flash_erase 0x800e5000 4096}] 1 "Unlock read failure must propagate"
assert_equal [expr {$regs(1073745936) & 128}] 128 "Unlock read failure must attempt relock"

agrv2k_flash_erase 0x800e5000 4096
assert_equal [expr {$regs(1073745936) & 128}] 128 "Erase must relock controller"
assert_equal $regs(1073745940) [expr {0x800e5000}] "Erase sector address"

set image [file join $temp test.bin]
set file [open $image wb]
set image_data ""
pack image_data 0xff00a9c2 -intle 32
pack image_data 0x6655 -intle 16 32
puts -nonewline $file $image_data
close $file
set writes {}
assert_equal [catch {agrv2k_flash_program $image 0x800e6ffc}] 1 "Padding must not cross CPLD boundary"
assert_equal [llength $writes] 0 "Cross-boundary image must not unlock flash"
agrv2k_flash_program $image 0x800e5000
assert_equal [llength $flash_calls] 1 "Program must delegate once to the native Flash driver"
assert_equal [lindex $flash_calls 0] [list write_image $image [expr {0x800e5000}] bin] "Native loader arguments"
assert_equal [llength $writes] 0 "No host-side per-word Flash writes"
assert_equal $verified 1 "Program must verify the original image"
set mock_flash_error 1
assert_equal [catch {agrv2k_flash_program $image 0x800e5000}] 1 "Native programming error must propagate"
assert_equal $verified 1 "Failed program must not be marked verified"
assert_equal [llength $writes] 0 "Native failure must not fall back to host programming"
set mock_flash_error 0
set mock_state running
assert_equal [catch {agrv2k_flash_program $image 0x800e5000}] 1 "Running target rejected before native call"
assert_equal [llength $flash_calls] 1 "Running target must not call native driver"
puts "AGRV2K_FLASH_CONTRACTS_PASS"
