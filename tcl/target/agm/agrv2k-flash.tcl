# SPDX-License-Identifier: GPL-2.0-or-later
# Explicit AGRV2K main-flash operations. Writes use the native SRAM FIFO loader.
# Registers: AGM SDK/Leego flash_agrv2k.c; transport: AGaMEMnon program.py.
# These procedures run only when explicitly called. No option-byte writes.

proc agrv2k_flash_bounds {address length} {
	global AGRV2K_FLASH_BASE AGRV2K_FLASH_SIZE AGRV2K_FLASH_WRITABLE_SIZE
	set address [expr {wide($address)}]
	set length [expr {wide($length)}]
	if {$length <= 0 || $address < $AGRV2K_FLASH_BASE ||
		$address >= $AGRV2K_FLASH_BASE + $AGRV2K_FLASH_SIZE ||
		$address >= $AGRV2K_FLASH_BASE + $AGRV2K_FLASH_WRITABLE_SIZE ||
		$length > $AGRV2K_FLASH_BASE + $AGRV2K_FLASH_SIZE - $address ||
		$length > $AGRV2K_FLASH_BASE + $AGRV2K_FLASH_WRITABLE_SIZE - $address} {
		error "AGRV2K flash range exceeds configured writable main-flash region"
	}
	return $address
}

proc agrv2k_flash_read32 {address} {
	return [lindex [read_memory $address 32 1] 0]
}

proc agrv2k_flash_wait {timeout {require_done 0}} {
	global AGRV2K_FLASH_CONTROLLER
	set deadline [expr {[clock milliseconds] + $timeout}]
	while {1} {
		set status [agrv2k_flash_read32 [expr {$AGRV2K_FLASH_CONTROLLER + 0x0c}]]
		if {!($status & 1) && (!$require_done || ($status & 0x34))} {
			if {$status & 0x14} {
				error [format "AGRV2K flash status error 0x%08x" $status]
			}
			return $status
		}
		if {[clock milliseconds] >= $deadline} {
			error "AGRV2K flash operation timed out"
		}
		sleep 1
	}
}

proc agrv2k_flash_prepare {} {
	global AGRV2K_TARGET AGRV2K_FLASH_CONTROLLER
	targets $AGRV2K_TARGET
	if {[$AGRV2K_TARGET curstate] ne "halted"} {
		error "AGRV2K must already be halted before a flash operation"
	}
	agrv2k_flash_wait 1000
	set ctrl [agrv2k_flash_read32 [expr {$AGRV2K_FLASH_CONTROLLER + 0x10}]]
	set idle [expr {$ctrl & ~0xd6ff}]
	if {$ctrl & 0x80} {
		set failed [catch {
			mww [expr {$AGRV2K_FLASH_CONTROLLER + 4}] 0x45670123
			mww [expr {$AGRV2K_FLASH_CONTROLLER + 4}] 0xcdef89ab
			set ctrl [agrv2k_flash_read32 [expr {$AGRV2K_FLASH_CONTROLLER + 0x10}]]
			if {$ctrl & 0x80} { error "AGRV2K flash unlock failed" }
		} failure]
		if {$failed} {
			catch {agrv2k_flash_lock $idle}
			error $failure
		}
	}
	return $idle
}

proc agrv2k_flash_lock {idle} {
	global AGRV2K_FLASH_CONTROLLER
	set status [agrv2k_flash_read32 [expr {$AGRV2K_FLASH_CONTROLLER + 0x0c}]]
	if {$status & 1} { error "AGRV2K flash remains busy; lock was not written" }
	mww [expr {$AGRV2K_FLASH_CONTROLLER + 0x10}] [expr {$idle | 0x80}]
}

proc agrv2k_flash_erase {address length} {
	global AGRV2K_FLASH_CONTROLLER AGRV2K_FLASH_SECTOR_SIZE
	set address [agrv2k_flash_bounds $address $length]
	if {$address % $AGRV2K_FLASH_SECTOR_SIZE || $length % $AGRV2K_FLASH_SECTOR_SIZE} {
		error "AGRV2K erase range must cover complete 4096-byte sectors"
	}
	set idle [agrv2k_flash_prepare]
	set failed [catch {
		for {set current $address} {$current < $address + $length} {incr current $AGRV2K_FLASH_SECTOR_SIZE} {
			mww [expr {$AGRV2K_FLASH_CONTROLLER + 0x0c}] 0x34
			mww [expr {$AGRV2K_FLASH_CONTROLLER + 0x14}] $current
			mww [expr {$AGRV2K_FLASH_CONTROLLER + 0x10}] [expr {$idle | 2}]
			sleep 1
			mww [expr {$AGRV2K_FLASH_CONTROLLER + 0x10}] [expr {$idle | 0x42}]
			agrv2k_flash_wait 1000 1
		}
	} failure]
	set lock_failed [catch {agrv2k_flash_lock $idle} lock_failure]
	if {$failed} { error $failure }
	if {$lock_failed} { error $lock_failure }
}

proc agrv2k_flash_program {filename address} {
	global AGRV2K_TARGET
	set file [open $filename rb]
	set failed [catch {set data [read $file]} failure]
	close $file
	if {$failed} { error $failure }
	set count [string bytelength $data]
	set address [agrv2k_flash_bounds $address [expr {($count + 3) & ~3}]]
	if {$address & 3} { error "AGRV2K write address must be word aligned" }
	targets $AGRV2K_TARGET
	if {[$AGRV2K_TARGET curstate] ne "halted"} {
		error "AGRV2K must already be halted before a flash operation"
	}
	# Native driver pads the final word, streams through SRAM, restores the
	# hart/work area and relocks on success and failure. No slow fallback.
	flash write_image $filename $address bin
	verify_image $filename $address bin
}
