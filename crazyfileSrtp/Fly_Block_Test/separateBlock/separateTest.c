#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include<math.h>
#include "app.h"

#include "FreeRTOS.h"
#include "task.h"
#include "system.h"
#include "semphr.h"
#include "radiolink.h"
#include "configblock.h"
#include "log.h"
#include "commander.h"
#include "stabilizer_types.h"
#include "param.h"
#define DEBUG_MODULE "P2P"
#include "debug.h"



/*---------这部分是使用到的宏变量的声明-----------*/

//广播任务的堆栈与优先级设置
#define RADIO_STACK_SIZE 1000
#define RADIO_MAIN_PRI 1

//增加邻居的线程等待时间
// #define WAIT_TIME_MS 300

//飞行方位点
// static setpoint_t setpoint;



/*----------------------------------------------------*/




/*---------这部分是使用到的结构体的声明-----------*/


//设置位置信息结构
struct Position
{
    float x;
    float y;
    float z;
    
};

//设置飞行方向
struct Direction
{
    float x;
    float y;
    float z;
    
};

//设置鸟类个体
struct Boid
{
    char id;                       //鸟的id,后期可用于分辨鸟是不是捕食者
    int baseRotationSpeed;         //基础旋转角速度
    int currRotaltionSpeed;        //当前旋转角速度
    float baseSpeed;               //基础速度
    float currSpeed;               //当前速度
    struct Position position;      //鸟的当前位置

    struct Direction currDirection;    //当前运动方向
    struct Direction desiredDirection; //预期运动方向
    
    struct Boid* nearbyBoidsArray;     //邻居链表
    int numNearbyArray;          //分离邻居的个数
    SemaphoreHandle_t neighborMutex;  // 互斥锁用于保护邻居链表
 
    SemaphoreHandle_t moveMutex;  // 互斥锁用于保护飞行执行逻辑

};

//独立发送的参数列表，传递给单独的进程任务
struct TaskParameters {
    struct Boid *boid;
    P2PPacket *p_reply;
};

/*----------------------------------------------------*/





/*--------这部分是对整个程序中使用到的所有全局变量-------*/


//全局化个体 鸟
struct Boid boid;

//全局化数据包
P2PPacket p_reply;


/*----------------------------------------------------*/


/*--------这部分是数据的基本运算部分-------*/

//坐标初始化，使用lighthouse获取
void InitPosition(struct Position position)
{
        logVarId_t idX = logGetVarId("stateEstimate", "x");
        float X = 0.0f;
        X = logGetFloat(idX);
        position.x = (double)X;

        logVarId_t idY = logGetVarId("stateEstimate", "y");
        float Y = 0.0f;
        Y = logGetFloat(idY);
        position.y = (double)Y;

        logVarId_t idZ = logGetVarId("stateEstimate", "z");
        float Z = 0.0f;
        Z = logGetFloat(idZ);
        position.z = (double)Z;
};

//方向初始化
void InitDirection(struct Direction direction)
{
   direction.x=0;
   direction.y=0;
   direction.z=0;
}

//鸟类个体初始化
void InitBoid(struct Boid* initBoid){
     DEBUG_PRINT("鸟类初始化\n");
     initBoid->id='A';
     initBoid->baseRotationSpeed =170;       
     initBoid->currRotaltionSpeed =170;
     initBoid->baseSpeed = 0.8;
     initBoid->currSpeed = 0.8;
     InitDirection(initBoid->currDirection);
     InitDirection(initBoid->desiredDirection);
     InitPosition(initBoid->position);
     initBoid->nearbyBoidsArray=NULL;
     initBoid->numNearbyArray=0;
     initBoid->neighborMutex = xSemaphoreCreateMutex();
     initBoid->moveMutex = xSemaphoreCreateMutex();

};

//计算两个个体之间的距离
float calculateDistance(struct Position pos1, struct Position pos2)
{
    float deltaX = pos1.x - pos2.x;
    float deltaY = pos1.y - pos2.y;
    float deltaZ = pos1.z - pos2.z;

    // 使用欧几里得距离公式计算距离
    float distance = sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);

    return distance;
}

//运动方向获取

struct Direction calculateDirection(struct Position startPosition, struct Position endPosition){
    struct Direction direction;

    // 计算始末位置之间的向量
    direction.x = endPosition.x - startPosition.x;
    direction.y = endPosition.y - startPosition.y;
    direction.z = endPosition.z - startPosition.z;

    // 计算向量的长度（模长）
    float length = sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);

    // 将向量归一化（单位化）
    if (length > 0.0f)
    {
        direction.x /= length;
        direction.y /= length;
        direction.z /= length;
    }
    else
    {
        // 如果长度为零，表示没有运动，方向保持不变
        direction.x = 0.0f;
        direction.y = 0.0f;
        direction.z = 0.0f;
    }

    return direction;
};

//判断是否在该周期内已经收过这只鸟发送的数据包

bool IsSameBoid(struct Boid curboid, struct Boid a) 
{
    for (int i = 0; i < curboid.numNearbyArray; i++) {
        if (curboid.nearbyBoidsArray[i].id == a.id) {
            // 如果找到相同的鸟，返回 false
            return false;
        }
    }
    // 如果没有找到相同的鸟，返回 true
    return true;
}


/*----------------------------------------------------*/





/*--------这部分是分布逻辑算法部分-------*/


// 在主任务中发送数据包
void sendDataPacketTask(void *pvParameters)
{
    struct TaskParameters *params = (struct TaskParameters *)pvParameters;
    struct Boid *boid = params->boid;
    P2PPacket *p_reply = params->p_reply;
    while (1)
    {
        // 获取坐标
        DEBUG_PRINT("重置飞行数据并开始广播\n");
        DEBUG_PRINT("重置飞行数据并开始广播\n");
        InitPosition(boid->position);



        // 将结构体 boid 序列化
        memcpy(&p_reply->data, &boid, sizeof(struct Boid));
        p_reply->size = sizeof(struct Boid);
        


        // 设置广播周期
        vTaskDelay(M2T(300));
        radiolinkSendP2PPacketBroadcast(p_reply);
         
        
        // 继续执行其他任务或延时
        vTaskDelay(M2T(100)); // 假设在发送数据包后等待1秒
    }
}

//自动判断邻居

// void addNearbyBoid(struct Boid* boid, struct Boid newNearbyBoid)
// {
//    if(xSemaphoreTake(boid->neighborMutex, pdMS_TO_TICKS(WAIT_TIME_MS)) == pdTRUE){

//     //创建一个新的扩容的数组存储原来的邻居与新的邻居
//     struct Boid* newNeightborArray = (struct Boid*)malloc((boid->numNearbyArray+1)*sizeof(struct Boid));


//     //复制现有的邻居到新的数组

//     for(int i=0;i<boid->numNearbyArray;i++){
//         newNeightborArray[i] = boid->nearbyBoidsArray[i];
//     };
    
//     //增加新邻居到新的数组

//     newNeightborArray[boid->numNearbyArray] = newNearbyBoid;

//     //释放原本的数组
    
//     free(boid->nearbyBoidsArray);
    
//     //更新邻居列表
//     boid->nearbyBoidsArray = newNeightborArray;
//     boid->numNearbyArray++;

//     // 释放互斥锁，允许其他线程访问邻居链表
//     xSemaphoreGive(boid->neighborMutex); 
//    }
//    else{
//     DEBUG_PRINT("此时链表不允许操作\n");
//    }
// }

// void removenearByboid(struct Boid* boid,struct Boid alreadyBoid)
// {
//     if(xSemaphoreTake(boid->neighborMutex, pdMS_TO_TICKS(WAIT_TIME_MS)) == pdTRUE){

//     //创建一个新的扩容的数组存储原来的邻居减去离开的邻居
//     struct Boid* newNeightborArray = (struct Boid*)malloc((boid->numNearbyArray-1)*sizeof(struct Boid));


//     //复制现有的邻居到新的数组,并删除离开的邻居

//     for(int i=0;i<boid->numNearbyArray-1;i++){
//         if(boid->nearbyBoidsArray[i].id!=alreadyBoid.id)
//         { 
//         newNeightborArray[i] = boid->nearbyBoidsArray[i];
//         };
//     };
    
//     //释放原本的数组
    
//     free(boid->nearbyBoidsArray);
    
//     //更新邻居列表
//     boid->nearbyBoidsArray = newNeightborArray;
//     boid->numNearbyArray--;

//     // 释放互斥锁，允许其他线程访问邻居链表
//     xSemaphoreGive(boid->neighborMutex); 
//    }
//    else{
//     DEBUG_PRINT("此时链表不允许操作\n");
//    }
// }



//先获取所有的邻居，此时没有分类
void p2pcallbackHandler(P2PPacket *p)
{

    
  //序列复制
    struct Boid copyBoid;
    memcpy(&copyBoid,&p->data[0], sizeof(struct Boid));
    
    //数据呈现
    DEBUG_PRINT("打印接收的数据包\n");
    DEBUG_PRINT("COPY_ID: %c\n", copyBoid.id);
    DEBUG_PRINT("COPY_Position_x: %f\n", (double)copyBoid.position.x);
    DEBUG_PRINT("COPY_Position_y: %f\n", (double)copyBoid.position.y);
    DEBUG_PRINT("COPY_Position_z: %f\n", (double)copyBoid.position.z);
    //设置两米之内都是邻居
    // if(IsSameBoid(boid.nearbyBoidsArray,copyBoid)==true)
    // {
    // double nearByDistance = calculateDistance(boid.position,copyBoid.position);
    // if(nearByDistance<=(double)2){
    //     addNearbyBoid(&boid,copyBoid);
    // }
    // else if(nearByDistance>(double)2){
    //     //即使这只鸟一开始不在邻居范围也不影响，只是增加了一部分的运算，返回原来的结果
    //     removenearByboid(&boid,copyBoid);
    // }
    // }
    // else {
    //     DEBUG_PRINT("该鸟已经存在邻居队列中\n");
    // }
}





/*----------------------------------------------------*/

//10.11问题出在全局变量的指针传递


void appMain()
{   



    //数据包初始化与端口设置
    p_reply.port = 0x00;
    InitBoid(&boid);
    

    // 创建结构体并初始化参数
    struct TaskParameters params;
    params.boid = &boid; // 指向 boid 的指针
    params.p_reply = &p_reply; // 指向 p_reply 的指针

    //回调函数注册
    p2pRegisterCB(p2pcallbackHandler);
     

    //将整个广播体系独立起来，相当于鸟类的声波器官
    xTaskCreate(sendDataPacketTask, "send_data_packet_task", RADIO_STACK_SIZE, &params, RADIO_MAIN_PRI, NULL);
    
    // while(1){
    //     move(&boid);
    // }
}