#include <fstream>//C++版本文件读写
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <openssl/md5.h>
#include "httplib.h"
#include "db.hpp"
using namespace std;

//回调函数,一个函数，调用时机由代码框架和操作系统来决定
//void Hello(const httplib::Request& req,httplib::Response& resp){
//  //HTTP Content-Type
//  resp.set_content("<h1>hello</h1>","text/html");
//}

void md5(const std::string& srcStr,std::string& md5value){
  //调用md5哈希
  unsigned char mdStr[33] = {0};
  MD5((const unsigned char*)srcStr.c_str(),srcStr.length(),mdStr);
  //哈希后的字符串
  md5value = std::string((const char*)mdStr);
  //哈希后的十六进制串 32字节
  char buf[65] = {0};
  char tmp[3] = {0};
    for(int i = 0;i < 32;++i){
      sprintf(tmp,"%02x",mdStr[i]);
      strcat(buf,tmp);
    }
    buf[32] = '\0';//后面都是0，从32字节截断
    md5value = std::string(buf);
}

class FileUtil{
  public:
    static bool Write(const std::string& file_name,const std::string& content){
      std::ofstream file(file_name.c_str());
      if(!file.is_open()){
        return false;
      }
      file.write(content.c_str(),content.length());
      file.close();
      return true;
    }

    static bool Read(const std::string& file_name,std::string* content){
      std::ifstream file(file_name.c_str());
      if(!file.is_open()){
        return false;
      }
      struct stat st;
      stat(file_name.c_str(),&st);
      content->resize(st.st_size);
      //一口气把整个文件都读完
      //需要先知道该文件的大小
      //read的第一个参数char* 缓冲区长度
      //第二个参数int 读取多长
      file.read((char*)content->c_str(),content->size());
      file.close();
      return true;
    }
};

MYSQL* mysql = NULL;
int main(){
  using namespace httplib;//引用命名空间，作用域在函数内部，在函数内部生效，避免名称冲突,不能放在头文件中
  mysql = image_system::MySQLInit();
  image_system::ImageTable image_table(mysql);
  signal(SIGINT,[](int){
      image_system::MySQLRelease(mysql);
      exit(0);    
      });

  Server server;
  //客户端请求 / 路径的时候，执行一个特定的函数
  //指定不同的路径对应到不同的函数上，这个过程称为”设置路由“
  // server.Get("/",[](const httplib::Request& req,httplib::Response& resp){
  //resp.set_content("<h1>hello</h1>","text/html");});

  //服务器中有两个重要概念：1.请求（Request）2.响应（Response）
  //[&image_table]这是 lambda 的重要特性，捕获变量
  //本来 lambda 内部是不能直接访问 image_table 的，捕捉之后就可以访问了，其中 & 的含义相当于按引用捕获
  server.Post("/image",[&image_table](const Request& req,Response& resp){
      Json::FastWriter writer;
      Json::Value resp_json;
      printf("上传图片!\n");
      //1.对参数进行校验
      auto ret = req.has_file("upload");
      if(!ret){
      printf("文件上传出错！\n");
      resp.status = 404;
      //可以使用json格式组织一个返回结果
      resp_json["ok"] = false;
      resp_json["reason"] = "上传文件出错，没有需要的upload字段"; 
      resp.set_content(writer.write(resp_json),"application/json");
      return;
      }
      //2.根据文件名获取到文件数据 file 对象
      const auto& file = req.get_file_value("upload");
      // file.filename;
      // file.content_type;
      //3.把图片的属性信息插入到数据库中
      Json::Value image;
      image["image_name"] = file.filename;
      image["size"] = (int)file.length;
      time_t tt;
      time(&tt);
      tt = tt + (8*3600);//transform the time zone
      tm* t = gmtime(&tt);
      char res[1024] = {0};
      sprintf(res,"%d-%02d-%02d %02d:%02d:%02d",t->tm_year+1900,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec);
      image["upload_time"] = res; 
      std::string md5value;
      auto body = req.body.substr(file.offset, file.length);
      md5(body,md5value);
      image["md5"] = md5value;
      image["type"] = file.content_type;
      image["path"] = "./data/" + file.filename;
      ret = image_table.Insert(image);
      if(!ret){
        printf("image_table Insert failed!\n");
        resp_json["ok"] = false;
        resp_json["reason"] = "数据库插入失败！";
        resp.status = 500;
        resp.set_content(writer.write(resp_json),"application/json");
        return;
      }

      //4.把图片保存到指定的磁盘目录中
      //body 图片内容
      FileUtil::Write(image["path"].asString(),body);

      //5.构造一个响应数据通知客户端上传成功
      resp_json["ok"] = true;
      resp.status = 200;
      resp.set_content(writer.write(resp_json),"application/json");
      return;
  });

  server.Get("/image",[&image_table](const Request& req,Response& resp){
      (void)req;//没有任何实际的效果，确认不需要使用req
      printf("获取所有图片信息\n");
      Json::Value resp_json;
      Json::FastWriter writer;
      //1.调用数据库接口来获取数据
      bool ret = image_table.SelectAll(&resp_json);
      if(!ret){
      printf("查询数据库失败！\n");
      resp_json["ok"] = false;
      resp_json["reason"] = "查询数据库失败！";
      resp.status = 500;
      resp.set_content(writer.write(resp_json),"application/json");
      return;
      }
      //2.构造响应结果返回给客户端
      resp.status = 200;
      resp.set_content(writer.write(resp_json),"application/json");
      });

  //1.正则表达式
  //2.原始字符串（raw string）C++11特性
  server.Get(R"(/image/(\d+))",[&image_table](const Request& req,Response& resp){
      Json::FastWriter writer;
      Json::Value resp_json;
      //1.先获取图片id
      //std::string matches1 = req.matches[1];
      //int image_id = atoi(matches1.c_str());
      //matches返回的是一个字符串，atoi是将c风格字符串转成数字,stoi是C++11中，将一个std::string转成数字
      int image_id = std::stoi(req.matches[1]);
      printf("获取id为 %d 的图片信息！\n",image_id);
      //2.再根据图片id查询数据库
      bool ret = image_table.SelectOne(image_id,&resp_json);
      if(!ret){
      printf("数据库查询出错！\n");
      resp_json["ok"] = false;
      resp_json["reason"] = "数据库查询出错!";
      resp.status = 404;
      resp.set_content(writer.write(resp_json),"application/json");
      return;
      }
      //3.把查询结果返回给客户端
      resp_json["ok"] = true;
      resp.set_content(writer.write(resp_json),"application/json");
      return;
  });

  server.Get(R"(/show/(\d+))",[&image_table](const Request& req,Response& resp){
      Json::FastWriter writer;
      Json::Value resp_json;
      //1.根据图片id去数据库查到对应的目录
      int image_id = std::stoi(req.matches[1]);
      printf("获取id为 %d 的图片内容！\n",image_id);
      Json::Value image;
      bool ret = image_table.SelectOne(image_id,&image);
      if(!ret){
      printf("读取数据库失败！\n");
      resp_json["ok"] = false;
      resp_json["reason"] = "数据库查询出错!";
      resp.status = 404;
      resp.set_content(writer.write(resp_json),"application/json");
      return;
      }
      //2.根据目录找到文件内容，读取文件内容
      std::string image_body;
      ret = FileUtil::Read(image["path"].asString(),&image_body);
      if(!ret) {
      printf("读取图片内容失败！\n");
      resp_json["ok"] = false;
      resp_json["reason"] = "读取图片内容失败！";
      resp.status = 500;
      resp.set_content(writer.write(resp_json),"application/json");
      return;
      }
      //3.把文件内容构造成一个响应
      resp.status = 200;
      //不同的图片设置的content-type是不一样的。
      resp.set_content(image_body,image["type"].asCString());
  });

  server.Delete(R"(/image/(\d+))",[&image_table](const Request& req,Response& resp){
      Json::FastWriter writer;
      Json::Value resp_json;
      //1.根据图片id到数据库中查到对应的目录
      int image_id = std::stoi(req.matches[1]);
      printf("删除id为 %d 的图片！\n",image_id);
      //2.查找到对应文件的路径
      Json::Value image;
      bool ret = image_table.SelectOne(image_id,&image);
      if(!ret){
      printf("删除图片文件失败！\n");
      resp_json["ok"] = false;
      resp_json["reason"] = "删除图片文件失败！";
      resp.status = 404;
      resp.set_content(writer.write(resp_json),"application/json");
      return;
      }
      //3.调用数据库操作进行删除
      ret = image_table.Delete(image_id);
      if(!ret){
      printf("删除图片文件失败！\n");
      resp_json["ok"] = false;
      resp_json["reason"] = "删除图片文件失败！";
      resp.status = 404;
      resp.set_content(writer.write(resp_json),"application/json");
      return;
      }
      //4.删除磁盘上的文件
      //C++17之前标准库中，没有删除文件的方法
      //此处只能使用操作系统提供的函数
      unlink(image["path"].asCString());
      //5.构造响应
      resp_json["ok"] = true;
      resp.status = 200;
      resp.set_content(writer.write(resp_json),"application/json");
  });

  server.set_base_dir("./wwwroot");//设置一个静态资源的目录

  server.listen("0.0.0.0",9094);//此表示了服务器启动的全部过程

  return 0;
}
