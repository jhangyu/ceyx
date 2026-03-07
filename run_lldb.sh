lldb ./test_color_accuracy -- ../../../../sample.dng test_out.bin << LLDB_EOF
break set -E C++
run
bt
LLDB_EOF
