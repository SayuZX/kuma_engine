#if defined(__APPLE__)
  .section __DATA,__flutterpay
  .globl ___flutter_payload_start
  .p2align 4
___flutter_payload_start:
  .incbin PAYLOAD_FILE
  .globl ___flutter_payload_end
___flutter_payload_end:
#else
  .section .flutter_payload,"a"
  .globl __flutter_payload_start
  .p2align 4
__flutter_payload_start:
  .incbin PAYLOAD_FILE
  .globl __flutter_payload_end
__flutter_payload_end:
#endif
