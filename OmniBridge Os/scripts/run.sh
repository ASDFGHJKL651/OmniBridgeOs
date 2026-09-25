cd d:/omnibridgeos/
make clean
make all
make -C tests/host clean
make -C tests/host all
scripts/merge_sources.py
make -C usr/lib/oblibc
scripts/obcc.sh usr/examples/pipe_test.c -o build/esp/bin/pipe_test.obr --minpriv 0
# 逐一编译测试程序
scripts/obcc.sh usr/examples/syscall_test.c    -o build/esp/bin/syscall_test.obr    --minpriv 0
scripts/obcc.sh usr/examples/signal_test.c     -o build/esp/bin/signal_test.obr     --minpriv 0
scripts/obcc.sh usr/examples/fs_test.c         -o build/esp/bin/fs_test.obr         --minpriv 0
scripts/obcc.sh usr/examples/tls_test.c        -o build/esp/bin/tls_test.obr        --minpriv 0
scripts/obcc.sh usr/examples/exception_test.c  -o build/esp/bin/exception_test.obr  --minpriv 0
make -C tests/host run
# 检查 ELF 类型与入口地址
readelf -h build/esp/bin/linux_hello.elf | grep -E 'Type|Entry'
# 期望：Type: EXEC          （不是 DYN！）
#       Entry point address: 0x80000....

# 检查 program headers
readelf -l build/esp/bin/linux_hello.elf | grep -E 'LOAD|INTERP|DYNAMIC'
# 期望：只有 PT_LOAD（可能还有 PT_GNU_STACK）
#       没有 PT_INTERP
#       没有 PT_DYNAMIC

# 大小确认
ls -l build/esp/bin/linux_hello.elf
scripts/run-qemu.sh