SECTIONS {
  .flutter_payload : ALIGN(16) {
    KEEP(*(.flutter_payload))
  }
}
INSERT AFTER .rodata;
