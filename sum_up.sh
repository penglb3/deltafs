#!/bin/bash
ops=(
"Directory creation"
"Directory stat"
"Directory rename"
"Directory removal"
"File creation"
"File stat"
"File read"
"File removal"
"Tree creation"
"Tree removal"
)

for op in "${ops[@]}"; do
	echo -n -e "${op}\t\t"
	grep "${op}" $1 | awk '{sum+=$3;n+=1;}END{print sum "\t" n}'
done
