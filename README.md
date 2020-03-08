# negative_cache_mgr

ATS 异常状态码缓存插件

参考 [ATS records.config Negative Response Caching](https://docs.trafficserver.apache.org/en/7.1.x/admin-guide/files/records.config.en.html#negative-response-caching)中定义的可缓存异常状态码，在该ATS插件中定义如下异常状态码的缓存功能：

| HTTP Response Code | Description |
| :----------------- | :---------- |
|     204            |  No Content |
|     305            |  Use Proxy  |
|     400	         | Bad Request |
|     403            | Forbidden |
|     404            | Not Found |
|     405            | Method Not Allowed |
|     414            | Request Url Too Long |
|     500            | Internal Server Error |
|     501            | Not Implemented |
|     502            | Bad Gateway |
|     503            | Service Unavailable |
|     504            | Gateway Timeout |


## Purpose

参考其他商用CDN厂商，如阿里CDN，支持针对某个异常状态码按用户需求自定义缓存时间。注意，这里说的异常状态码，是指回源得到的异常状态码，而非ATS本身响应的。

注：此插件不支持对416异常状态码的缓存，对416的缓存功能，应该在 `cache_range_requests.so` 插件中实现。

## Configuration

此插件为非全局插件，即不可以在`plugin.config`中定义该插件，只能在`remap.config`中以域名粒度生效。

### Option

默认情况下，如果开启了该插件，但是没有指定任何参数，则异常状态码的缓存规则遵循`records.config`中的定义。

- `negative_cache_enable`: 指定需要自定义缓存的异常状态码，如果存在多个，以分号（;）分隔开。
- `negative_cache_time`: 指定需要自定义缓存的异常状态码对应的缓存时间，单位为秒，由于是异常状态码的缓存，建议最大的缓存时间不要超过86400。如果存在多个，以分号（;）隔开。这里与异常状态码的位置为一一对应的关系。

### Examples

```
map  https://foo.com/ https://bar.foo.com/   @plugin=negative_cache_mgr.so @pparam=--negative_cache_enable=403;404;504 @pparam=--negative_cache_time=20;30;40
```

## Implementation Notes

该插件的设计和实现思路来源于 `cache_range_requests` 和 [cache status](https://github.com/oxwangfeng/trafficserver-plugin/blob/master/cache_status/cache_status.cc) 两个插件。在ATS发起回源请求后，根据origin response header中的内容，将待缓存的异常状态码的status临时改为200，以便ATS根据CacheKey来进行object的缓存，并打上`tmp_cache_XXX`的tag；在ATS send_response to client阶段，根据`tmp_cache_XXX`的tag，提取出异常状态码，并修改response header，发送给客户端。

## Compile

### ATS v7.X

1. 需要修改 `include` 内容，将如下：

    ```
    #include "tscore/ink_defs.h"
    #include "tscore/ink_memory.h"
    ```

    改为：

    ```
    #include "ts/ink_defs.h"
    #include "ts/ink_memory.h"
    ```

2. 利用ATS自带的编译工具，txsx进行编译

    ```
    cd plugins/experimental/negative_cache_mgr
    /home/cdn/ats/ats/bin/tsxs -I ../../../lib/ -o negative_cache_mgr.so negative_cache_mgr.cc
    ```


### ATS v8.X

利用ATS自带的编译工具，txsx进行编译:

```
cd plugins/experimental/negative_cache_mgr
/home/cdn/ats/ats/bin/tsxs -I ../../../include/ -o negative_cache_mgr.so negative_cache_mgr.cc
```