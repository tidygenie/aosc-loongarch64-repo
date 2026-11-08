# Read-only except GDB watchpoints; require gsgpu_dri.so SHA-256
# aff7101ab877db0026fa0f8274e03a579f1b4fd2c16fabe2478d08f224d0da0a.
# Watches drawcall+336 after the first UV descriptor, expecting 0x010c2b00.
set pagination off
set confirm off
set breakpoint pending on
break __driDriverGetExtensions_gsgpu
run
set $gsgpu_base = (unsigned long)&__driDriverGetExtensions_gsgpu - 0x7cbc0
set $desc_ready = 0
disable 1
break *($gsgpu_base + 0x17bcbc)
condition 2 $s8 == 2
commands 2
 silent
 set $desc_ready = 1
 printf "UV_DESCRIPTOR_READY=0x%lx\n", $s4
 disable 2
 continue
end
break *($gsgpu_base + 0x197e3c)
commands 3
 silent
 if $desc_ready == 1
  set $drawcall = $s2
  set $pointer_slot = $drawcall + 336
  printf "WATCH_IB_DESCRIPTOR_SLOT drawcall=0x%lx slot=0x%lx\n", $drawcall, $pointer_slot
  disable 3
  watch *(unsigned int*)$pointer_slot
  commands 4
   silent
   if *(unsigned int*)$pointer_slot == 0x010c2b00
    printf "DESCRIPTOR_POINTER_WRITER pc=0x%lx offset=0x%lx value=0x%x\n", $pc, $pc - $gsgpu_base, *(unsigned int*)$pointer_slot
    x/12i $pc - 20
    disable 4
   else
    continue
   end
  end
 end
 continue
end
continue
continue
