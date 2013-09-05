#!/bin/sh

# �� shell �����������ð����ƴ洢�������ļ�.
# > ���� /etc/fmc.conf.d/aliyun_oss.conf �ļ�
# > ��ȡ ak
# > ��ȡ sk

oss_dir="/etc/fmc.conf.d"
oss_file="/etc/fmc.conf.d/aliyun_oss.conf"
null_file="/dev/null"

trap 'echo ; exit 1' INT

# ���������ļ�����ӦĿ¼
create_oss_file() {
	if [ -d $oss_dir ] ; then # Ŀ¼����
		:
	else
		mkdir $oss_dir	# ����Ŀ¼
		echo "Create directory $oss_dir ... Success. "
	fi

	touch $oss_file	# �����ļ�
	echo "Creating file $oss_file ... Success. "; echo

	return 0
}

echo
if [ -f $oss_file ]; then # �ļ�����

	echo -n "$oss_file already exist, do you want to replace it?[y/n]: "
	read replace_file

	if [ -z $replace_file ] || [ $replace_file = "y" ] # �滻�ļ�
	then
		cat $null_file > $oss_file
	else
		echo Error: aliyun_oss is not installed.
		exit 1	
	fi
	echo
else # �ļ�������
	echo -n "$oss_file not exist, do you want to create it?[y/n]: "
	read create_file

	if [ -z $create_file ] || [ $create_file = "y" ]
	then
		create_oss_file # �����ļ�
	else
		echo Error: aliyun_oss is not installed.
		exit 1	
	fi
fi

echo -n Please input your access key: # ���� ak
read ak
if [ -z $ak ]; then
	echo Error: access key cannot be empty!
	exit 1
fi

echo -n Please input your secret key: # ���� sk
read sk
if [ -z $sk ]; then
	echo Error: secret key cannot be empty!
	exit 1
fi

echo "ak=$ak" >> $oss_file
echo "sk=$sk" >> $oss_file

echo; echo Install aliyun_oss success! ;echo
exit 0




