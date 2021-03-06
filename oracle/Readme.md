# oracle的一些使用技巧

## 创建单个表空间
```sql
create tablespace ts_test 
nologging 
datafile '/opt/oracle/oradata/ora/data.dbf' 
size 100m 
reuse 
autoextend on 
next 100m maxsize 1000m 
extent management local 
segment space management auto;
```
## 创建多个表空间
```sql
create tablespace ts_test 
nologging 
datafile 
'/opt/oracle/oradata/ora/data01.dbf' size 100m reuse autoextend on next 100m maxsize 1000m,
'/opt/oracle/oradata/ora/data02.dbf' size 100m reuse autoextend on next 100m maxsize 1000m,
'/opt/oracle/oradata/ora/data03.dbf' size 100m reuse autoextend on next 100m maxsize 1000m,
'/opt/oracle/oradata/ora/data04.dbf' size 100m reuse autoextend on next 100m maxsize 1000m 
extent management local 
segment space management auto;
```
## 创建表空间时使用bigfile（创建大容量表空间）
```sql
create bigfile tablespace ts_test 
nologging 
datafile '/home/oracle/data.dbf' 
size 100g 
reuse 
autoextend on 
next 10g maxsize 1000g 
extent management local 
segment space management auto;
```
## 创建用户并指定默认表空间
```sql
create user cd identified by Huawei123 default tablespace ts_test;
```
## 给用户赋权限
```sql
--赋予dba权限
grant connect,resource,dba to cd;
--只赋予针对某张表的只读权限，其中XXX是某个表的表名
grant connect,select on XXX to cd;
```
## 针对某张表启用nologging
```sql
alter table XXX nologging;
```
## 在sqlplus中执行sql脚本
```shell
SQL> @test.sql;
```
## 导入导出
```sql
--导出某些表
exp 用户名/密码@ip:port/SID file=/xx/test.dmp tables=XX,YY compress=y
--导出整个用户下的所有东西
exp 用户名/密码@ip:port/SID file=/xx/test.dmp full=y compress=y
--导入
imp 用户名/密码@ip:port/SID file=/xx/test.dmp tables=XX,YY
```
## 快速将文本导入表
```sql
--方法1
insert into XXX (id,name)
select 1,'a' from dual union all
select 2,'b' from dual union all
select 3,'c' from dual;

--方法2
--使用oracle自带的工具sqlldr
--sqlldr用法示例：
--1. 假设存在一张如下的表
create table user (
  id number,
  name varchar2(20)
)
--2. 创建一个文本文件test.txt，用于存放需要插入上表的数据
  cat test.txt
  1,张三
  2,李四
  3,王五
--3. 创建一个控制文件test.ctl，用于控制sqlldr的行为
  cat test.ctl
  load data
  infile 'test.txt'
  insert into table user
  fields terminated by ","
  (id,name)
--4. 在oracle用户下执行
sqlldr userid=userName/passwd@oracleIP:oraclePort/oracleSID control=test.ctl silent=header,feedback
--如果有多个文件，可以多进程后台执行sqlldr
```
## 检查用户表空间是否存在
```sql
select * from v$dbfie order by 1;
```
## 清理用户和表空间
```sql
--1. 删除用户的表空间
drop tablespace XXX including contents and datafiles;
--2. 删除用户及其关联项：
drop user XXX cascade;
```
## oracle存储过程
```sql
CREATE OR REPLACE PROCEDURE XXX
AS
  --定义数组，如果是中文字符，注意每个占3个字节
  type array is table of VARCHAR2(400);
  test_array := array('人','口','手');
  --变量定义
  id VARCHAR2(8);
  v_t1 timestamp(6);
  ...
BEGIN
  --执行sql命令
  execute immediate 'alter session set nls_date_language=american';
  a := '1';
  v_t1 := to_timestamp('01-JAN-18 01.00.01.009000000 AM', 'DD-MON-RR HH.MI.SSXFF AM');
  ...
FOR i IN 1..10
LOOP
  --随i自增，共8位，左补0
  id := lpad(i, 8, 0);
  IF MOD(i, 2) = 0 THEN
    ...
  ELSE
    ...
  END IF;
  
  --2位大写字母+4个数字，U表示大写，X表示大写或者数字，随机整数：trunc(dbms_random.value(0, XXX))
  b := dbms_random.string('U', 2) || lpad(trunc(dbms_random.value(0, 9999)), 4, 0);
  --取数组元素
  c := test_array(2);
  
  --if-else if
  IF XX THEN
    ...
  ELSE IF XX THEN
    ...
  ELSE IF XX THEN
    ...
  END IF;
  END IF;
  END IF;
  
  --对于blob类型，假设d是blob类型
  d := HEXTORAW('十六进制字符串');
  
  INSERT INTO XX values (id, xx, xx, ...);
  
  IF MOD(i, 10000) = 0 THEN
    COMMIT;
  END IF;
END LOOP;
COMMIT;

END;
```
## oracle快速插入亿级数据
```sql
CREATE OR REPLACE PROCEDURE XXX AS
BEGIN
  FOR i IN 1..100000000 LOOP
    --启用append，不写日志，并行插入，并行处理的线程数为10
    INSERT INTO /*+ append parallel(T_TEST,10) nologging */ T_TEST values(i);
    --每100万条commit一次
    IF MOD(i,1000000) = 0 THEN
      COMMIT;
    END IF;
  END LOOP;
  COMMIT;
END;
```
## oracle连接数
```sql
--查看当前连接数
select count(1) from v$session;
--查看并发连接数
select count(1) from v$session where status='ACTIVE';
--查看数据库允许的最大连接数
select value from v$parameter where name = 'processes';
--查看数据库允许的最大连接数
show parameter processes;
--查看不同用户的连接数
select username,count(username) from v$session where username is not null group by username;

--修改连接数，需要重启oracle生效
alter system set processes = 300 scope = spfile;
shutdown immediate;
startup;
```

## 通过直接拷贝数据文件实现oracle快速迁移
假设源和目的oracle分别为A和B
1.B安装oracle，B的主机名、数据库实例名、安装目录都和A保持一致
2.在A查询需要拷贝的文件
```shell
sqlplus / as sysdba
```
```text
SQL> show parameter pfile
NAME     TYPE VALUE
------------------------------------ ----------- ------------------------------
spfile    string /u01/app/oracle/product/11.2.0/dbhome_1/dbs/spfileorcl.ora
SQL> show parameter control
NAME     TYPE VALUE
------------------------------------ ----------- ------------------------------
control_file_record_keep_time integer 7
control_files   string /u01/app/oracle/oradata/orcl/control01.ctl, /u01/app/oracle/recovery_area/orcl/control02.ctl
control_management_pack_access string DIAGNOSTIC+TUNING
SQL> select * from v$logfile;
 GROUP# STATUS TYPE MEMBER          IS_RECOVERY_DEST_FILE
---------- ------- ------- -------------------------------------------------------------------------------- ---------------------
  3  ONLINE /u01/app/oracle/oradata/orcl/redo03.log      NO
  2  ONLINE /u01/app/oracle/oradata/orcl/redo02.log      NO
  1  ONLINE /u01/app/oracle/oradata/orcl/redo01.log      NO
SQL> select name from v$datafile;
NAME
--------------------------------------------------------------------------------
/u01/app/oracle/oradata/orcl/system01.dbf
/u01/app/oracle/oradata/orcl/sysaux01.dbf
/u01/app/oracle/oradata/orcl/undotbs01.dbf
/u01/app/oracle/oradata/orcl/users01.dbf
/u01/app/oracle/oradata/orcl/users02.dbf
SQL> select name from v$tempfile;
NAME
--------------------------------------------------------------------------------
/u01/app/oracle/oradata/orcl/temp01.dbf
```
可见需要拷贝的文件有如下一些：
```text
/u01/app/oracle/product/11.2.0/dbhome_1/dbs/spfileorcl.ora
/u01/app/oracle/oradata/orcl/control01.ctl
/u01/app/oracle/recovery_area/orcl/control02.ctl
/u01/app/oracle/oradata/orcl/redo03.log
/u01/app/oracle/oradata/orcl/redo02.log
/u01/app/oracle/oradata/orcl/redo01.log
/u01/app/oracle/oradata/orcl/system01.dbf
/u01/app/oracle/oradata/orcl/sysaux01.dbf
/u01/app/oracle/oradata/orcl/undotbs01.dbf
/u01/app/oracle/oradata/orcl/users01.dbf
/u01/app/oracle/oradata/orcl/users02.dbf
/u01/app/oracle/oradata/orcl/users03.dbf
/u01/app/oracle/oradata/orcl/temp01.dbf
```
3.停掉A、B
```shell
SQL>shutdown immediate;
```
4.使用scp命令，逐一将需要拷贝的文件从A拷贝到B，下面的示例中192.168.1.18表示B
```shell
scp /u01/app/oracle/product/11.2.0/dbhome_1/dbs/spfileorcl.ora oracle@192.168.1.18:/u01/app/oracle/product/11.2.0/dbhome_1/dbs/spfileorcl.ora
scp /u01/app/oracle/oradata/orcl/control01.ctl oracle@192.168.1.18:/u01/app/oracle/oradata/orcl/control01.ctl
scp /u01/app/oracle/recovery_area/orcl/control02.ctl oracle@192.168.1.18:/u01/app/oracle/recovery_area/orcl/control02.ctl
scp /u01/app/oracle/oradata/orcl/redo03.log oracle@192.168.1.18:/u01/app/oracle/oradata/orcl/redo03.log
scp /u01/app/oracle/oradata/orcl/redo02.log oracle@192.168.1.18:/u01/app/oracle/oradata/orcl/redo02.log
scp /u01/app/oracle/oradata/orcl/redo01.log oracle@192.168.1.18:/u01/app/oracle/oradata/orcl/redo01.log
scp /u01/app/oracle/oradata/orcl/system01.dbf oracle@192.168.1.18:/u01/app/oracle/oradata/orcl/system01.dbf
scp /u01/app/oracle/oradata/orcl/sysaux01.dbf oracle@192.168.1.18:/u01/app/oracle/oradata/orcl/sysaux01.dbf
scp /u01/app/oracle/oradata/orcl/undotbs01.dbf oracle@192.168.1.18:/u01/app/oracle/oradata/orcl/undotbs01.dbf
scp /u01/app/oracle/oradata/orcl/users01.dbf oracle@192.168.1.18:/u01/app/oracle/oradata/orcl/users01.dbf
scp /u01/app/oracle/oradata/orcl/users02.dbf oracle@192.168.1.18:/u01/app/oracle/oradata/orcl/users02.dbf
scp /u01/app/oracle/oradata/orcl/users03.dbf oracle@192.168.1.18:/u01/app/oracle/oradata/orcl/users03.dbf
scp /u01/app/oracle/oradata/orcl/temp01.dbf oracle@192.168.1.18:/u01/app/oracle/oradata/orcl/temp01.dbf
```
5.启动B
```shell
sqlplus / as sysdba
```
```text
SQL> startup
```
检查数据库的状态是否是open
```text
SQL> select status from v$instance;
STATUS
------------
OPEN
```
如果不是open，再依次执行下面两句
```text
SQL> recover database;
SQL> alter database open;
```
