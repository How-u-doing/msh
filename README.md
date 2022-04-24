# msh
My (Minimum) UNIX SHell

Currently implemented features include
- redirections (<, >, 2>, >>, 2>>, 2>&1, &>, &>>)
- pipelines (e.g. du --max-depth=1 ~ -h | sort -h > stat.txt)
- job control (signals, process control, job lists)

To be added
- wildcards
- autocomplete
- etc.
