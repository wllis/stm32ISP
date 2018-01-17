#include <stdio.h>
#include <unistd.h>
#include <wiringSerial.h>

int fd;

typedef struct {
    unsigned char bootloaderversion;    //1字节，bootloader版本
    unsigned char cmd_count;    //支持的所有指令个数
    unsigned char cmd[16];      //支持的所有指令
    unsigned int PID;       //product ID
} stm32info_t;
stm32info_t stm32info;

static int waitACK()    //等待接收数据，第一个字节如果是0x79则认为ok返回1，如果是0x1f则认为失败返回0
{
    while(1){
        if(0 != serialDataAvail(fd)) {
            if(0x79 == serialGetchar(fd))
                return 1;
            else
                return 0;
        }
        usleep(1000);
    }
}

int stm32_sync()
{
    printf("stm32_sync\n");
    serialFlush(fd);
    while(1) {
        serialPutchar(fd, 0x7f);
        if(waitACK()) {
            printf("sync ok\n");
            return 1;
        }
        usleep(10000);
    }
    serialFlush(fd);
}
int stm32_get_command()
{
    unsigned char get[32];
    int i, j;
    printf("%s\n", __func__);
    serialPutchar(fd, 0x00);
    serialPutchar(fd, 0xff);
    waitACK();
    //下面开始接收数据
    i=0;
    while(1) {
        get[i] = serialGetchar(fd);     //读取数据，如果没有数据的话则会等待
        if(get[i] == 0x79)      //结束
            break;
        else if(get[i] == 0x1f)
            return -1;
        i++;
    }
    //数据处理
    stm32info.bootloaderversion = get[1];
    stm32info.cmd_count = get[0];
    for(j=0;j < stm32info.cmd_count;j++)
        stm32info.cmd[j] = get[j+2];

    serialFlush(fd);
    return 1;
}
int stm32_get_ID_command()
{
    unsigned char get[16];
    int i, j;
    printf("%s\n", __func__);
    serialPutchar(fd, 0x02);
    serialPutchar(fd, 0xfd);
    waitACK();
    //接收数据
    serialGetchar(fd);
    stm32info.PID = serialGetchar(fd);
    stm32info.PID <<= 8;
    stm32info.PID += serialGetchar(fd);
    waitACK();
    
    serialFlush(fd);
    return 1;    
}
int stm32_erase_all()
{
    printf("%s\n", __func__);
    serialPutchar(fd, 0x43);
    serialPutchar(fd, 0xbc);
    waitACK();
    serialPutchar(fd, 0xff);
    serialPutchar(fd, 0x00);
    waitACK();
    printf("erase flash done\n");
    serialFlush(fd);
    return 1;
}
static unsigned char checksum(unsigned char *data, int len)      //计算p开始len个字节的checksum，也就是计算异或
{
    int i;
    unsigned char cs;
    cs = 0;
    for ( i=0; i<len; i++ )
        cs ^= data[i];
    return cs;
}
static int stm32_write_block(unsigned char *data, unsigned int addr, int len)
    //写入数据块，从*data处，往stm32的addr处，写入len字节数据，len最大256
{
    unsigned char temp[4];      //保存addr的四个字节
    unsigned char len1;
    int i;
    temp[0] = ((addr>>24) & 0xff);
    temp[1] = ((addr>>16) & 0xff);
    temp[2] = ((addr>>8) & 0xff);
    temp[3] = ((addr) & 0xff);

    serialPutchar(fd, 0x31);
    serialPutchar(fd, 0xce);
    waitACK();
    serialPutchar(fd, temp[0]);
    serialPutchar(fd, temp[1]);
    serialPutchar(fd, temp[2]);
    serialPutchar(fd, temp[3]);
    serialPutchar(fd, checksum(temp, 4));
    waitACK();
    //下面发送数据
    len1 = (unsigned char)(len - 1);
    serialPutchar(fd, len1);
    for(i=0;i<len;i++){
        serialPutchar(fd, data[i]);
    }
    serialPutchar(fd, len1 ^ checksum(data, len));
    waitACK();

    serialFlush(fd);
    usleep(1000);
    return 1;
}
static int stm32_read_block(unsigned char *data, unsigned int addr, int len)
{
    unsigned char temp[4];      //保存addr的四个字节
    unsigned char len1;
    int i;
    temp[0] = ((addr>>24) & 0xff);
    temp[1] = ((addr>>16) & 0xff);
    temp[2] = ((addr>>8) & 0xff);
    temp[3] = ((addr) & 0xff);

    serialPutchar(fd, 0x11);
    serialPutchar(fd, 0xEE);
    waitACK();
    serialPutchar(fd, temp[0]);
    serialPutchar(fd, temp[1]);
    serialPutchar(fd, temp[2]);
    serialPutchar(fd, temp[3]);
    serialPutchar(fd, checksum(temp, 4));
    waitACK();
    //下面发送要读取的字节数
    len1 = (unsigned char)(len - 1);
    serialPutchar(fd, len1);
    serialPutchar(fd, ~len1);
    waitACK();
    //下面接收数据
    for(i=0;i<len;i++)
        data[i] = serialGetchar(fd);
}

int main(int argc, char *argv[])
{
    unsigned char get[100];
    int i;

    printf("open\n");
    fd = serialOpen("/dev/ttyUSB0", 57600, 8, 1, 'N', 30);
    
    stm32_sync();
    
    stm32_get_command();
    printf("get bootloader version is 0x%x\n", stm32info.bootloaderversion);
    printf("get %d support commands: ", stm32info.cmd_count);
    for(i=0;i<stm32info.cmd_count;i++)
        printf("0x%x ", stm32info.cmd[i]);
    printf("\n");

    stm32_get_ID_command();
    printf("get PID is 0x%x\n", stm32info.PID);

    stm32_erase_all();

    //打开文件并下载
    printf("\nstarting download %s\n", argv[1]);
    {
        unsigned char buf[256];
        int i, offset;
        FILE *fp=NULL;
        fp = fopen(argv[1], "rb");

        i=0;
        offset = 0;
        while(1){
            fread(&buf[i], 1, 1, fp);       //采用一个字节一个字节读取，方便检测文件结尾
            if(feof(fp)) break;
            i++;
            if(i==256)  //一个块读取完成，把这个块发送出去
            {
                printf("writing data %5d\n\r\033[1A", offset);
                stm32_write_block(buf, 0x08000000+offset, 256);
                i = 0;
                offset+=256;
            }
        }
        printf("writing data %5d\n", offset);
        stm32_write_block(buf, 0x08000000+offset, i);
        fclose(fp);
    }
    printf("download succeed\n\n");

    printf("starting verify\n");
    {
        unsigned char buf[256];
        unsigned char buf_read[256];
        int i,j, offset;
        FILE *fp=NULL;
        fp = fopen(argv[1], "rb");
        
        i=0;
        offset=0;
        while(1){
            fread(&buf[i], 1, 1, fp);       //采用一个字节一个字节读取，方便检测文件结尾
            if(feof(fp)) break;
            i++;
            if(i==256)  //一个块读取完成，从stm32里也读出一个块出来
            {
                stm32_read_block(buf_read, 0x08000000+offset, 256);
                printf("verifying data %5d\n\r\033[1A", offset);
                for(j=0;j<256;j++)
                    if(buf[j] != buf_read[j]) printf("verify fail\n");
                i = 0;
                offset+=256;
            }
        }
        stm32_read_block(buf_read, 0x08000000+offset, i);
        printf("verifying data %5d\n", offset);
        for(j=0;j<i;j++)
            if(buf[i] != buf_read[i]) printf("verify fail\n");
        fclose(fp);
    }
    printf("verify succeed\n");

    serialClose(fd);
    return 0;
}
