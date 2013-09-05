#!/bin/sh

# 此 shell 程序用于设置阿里云存储的配置文件.
# > 创建 /etc/fmc.conf.d/aliyun_oss.conf 文件
# > 读取 ak
# > 读取 sk

oss_dir="/etc/fmc.conf.d"
oss_file="/etc/fmc.conf.d/aliyun_oss.conf"
null_file="/dev/null"

trap 'echo ; exit 1' INT

# 创建配置文件和相应目录
create_oss_file() {
	if [ -d $oss_dir ] ; then # 目录存在
		:
	else
		mkdir $oss_dir	# 创建目录
		echo "Create directory $oss_dir ... Success. "
	fi

	touch $oss_file	# 创建文件
	echo "Creating file $oss_file ... Success. "; echo

	return 0
}

echo
if [ -f $oss_file ]; then # 文件存在

	echo -n "$oss_file already exist, do you want to replace it?[y/n]: "
	read replace_file

	if [ -z $replace_file ] || [ $replace_file = "y" ] # 替换文件
	then
		cat $null_file > $oss_file
	else
		echo Error: aliyun_oss is not installed.
		exit 1	
	fi
	echo
else # 文件不存在
	echo -n "$oss_file not exist, do you want to create it?[y/n]: "
	read create_file

	if [ -z $create_file ] || [ $create_file = "y" ]
	then
		create_oss_file # 创建文件
	else
		echo Error: aliyun_oss is not installed.
		exit 1	
	fi
fi

echo -n Please input your access key: # 设置 ak
read ak
if [ -z $ak ]; then
	echo Error: access key cannot be empty!
	exit 1
fi

echo -n Please input your secret key: # 设置 sk
read sk
if [ -z $sk ]; then
	echo Error: secret key cannot be empty!
	exit 1
fi

echo "ak=$ak" >> $oss_file
echo "sk=$sk" >> $oss_file

echo; echo Install aliyun_oss success! ;echo
exit 0




