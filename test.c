#include <stdio.h> 
#include "async_logger.h" 

void test(){
  LOG_INFO("log in function");
}
int main()
{
  async_logger_init("module1", LOG_LVL_INFO);
  LOG_WARN("this is a  warn:%d",520);
  LOG_INFO("this is a  info:%s","hehe");
  LOG_DEBUG("this is a  debug:%d",520);

  test();
  return 0;
}
