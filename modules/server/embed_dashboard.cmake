# Turns dashboard/home_dashboard.html into a C++ include: adjacent raw string literals of <= 8000 bytes each
# (MSVC rejects longer literals; adjacent literals concatenate). The file must be ASCII.
file(READ "${FILE}" content)
string(LENGTH "${content}" len)
set(pos 0)
set(out "")
while(pos LESS len)
  string(SUBSTRING "${content}" ${pos} 8000 slice)
  string(APPEND out "R\"DASHRAW(${slice})DASHRAW\"\n")
  math(EXPR pos "${pos} + 8000")
endwhile()
file(WRITE "${OUT}" "${out}")
