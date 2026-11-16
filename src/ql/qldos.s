| OpenQL's DOS1_ driver for the QL: a directory device in the ROM port
| whose open, input/output and close are done by openql itself (qldos.c).
| Each call into openql is a line A word, $A0D0 + n, which the emulator
| takes in place of the exception. Assembled by tools/build-qldos-rom into
| src/ql/qldos_rom.h. MIT licence, as the rest of OpenQL.
|
| QDOS references: a directory driver's linkage (ch_next, ch_inout,
| ch_open, ch_close, ch_slave, ch_renam, -, ch_formt, ch_dflen, ch_drnam),
| file channels from $18 (fs_next $18, fs_drive $1D), the physical
| definition's fs_files at $22, sv_fsdef $100 and sv_fslst $140, and the
| vectors MM.RECHP $C2 and UT.UNLNK $D4.

        .text
base:
        .long   0x4afb0001              | a ROM
        .word   0                       | no SuperBASIC procedures
        .word   init-base
        .word   name_end-name
name:   .ascii  "OpenQL DOS1_: the host folder\n"
name_end:
        .even

init:
        movem.l %d0-%d3/%a0-%a3,-(%sp)
        moveq   #0x18,%d0               | MT.ALCHP: the linkage, in the common heap
        moveq   #0x30,%d1
        moveq   #0,%d2
        trap    #1
        tst.l   %d0
        bne.s   init_done
        lea     io(%pc),%a1
        move.l  %a1,4(%a0)
        lea     open(%pc),%a1
        move.l  %a1,8(%a0)
        lea     close(%pc),%a1
        move.l  %a1,12(%a0)
        lea     nothing(%pc),%a1
        move.l  %a1,16(%a0)             | slaving: there are no slave blocks
        move.l  %a1,20(%a0)
        lea     format(%pc),%a1
        move.l  %a1,28(%a0)
        moveq   #0x40,%d1
        move.l  %d1,32(%a0)             | the physical definition's length
        move.w  #3,36(%a0)
        move.l  #0x444f5300,38(%a0)     | "DOS"
        moveq   #0x22,%d0               | MT.LDD
        trap    #1
init_done:
        movem.l (%sp)+,%d0-%d3/%a0-%a3
        rts

open:                                   | a0 the channel, a1 the physical definition, d3 the key
        .word   0xa0d1
        rts

io:                                     | a0 the channel, d0 the operation
        .word   0xa0d2
nothing:
        rts

format:
        moveq   #-19,%d0                | ERR.NI
        rts

close:                                  | a0 the channel, a6 the system variables
        .word   0xa0d3                  | openql writes the file back if it changed
        moveq   #0,%d0
        move.b  0x1d(%a0),%d0           | fs_drive
        lsl.w   #2,%d0
        lea     0x100(%a6),%a1          | sv_fsdef
        move.l  0(%a1,%d0.w),%a1        | the physical definition
        subq.b  #1,0x22(%a1)            | one file fewer open there
        lea     0x18(%a0),%a0           | fs_next
        lea     0x140(%a6),%a1          | sv_fslst
        move.w  0xd4.w,%a2              | UT.UNLNK
        jsr     (%a2)
        lea     -0x18(%a0),%a0
        move.w  0xc2.w,%a2              | MM.RECHP
        jmp     (%a2)
