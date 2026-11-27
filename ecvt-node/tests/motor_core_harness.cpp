#include "motor/motor_core.h"
#include "servo/ECVT_motor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

struct Idle {};
static void (*owner)(void *);
static std::vector<unsigned char> queued;
static unsigned item_size;
static int64_t now=1000000;
static uint16_t drive_status=0x0250;
static uint8_t drive_mode=3, errors;
static int32_t position=4999, target_position;
static uint32_t target_velocity=999, parameter_value=0xfedcba98u;
static bool read_fails, write_fails, stuck, cancel_move, moving, cancel_snapshot;
static uint32_t requested_stop;
static unsigned sample_reads;
static std::vector<std::string> bus;
static std::vector<MotorResult> completed;

const char *esp_err_to_name(int) {return "test";}
int64_t esp_timer_get_time() {return now;}
void feedback() {
    servo_feedback_record(1,drive_status,++now);
    servo_feedback_record(2,position,++now);
    servo_feedback_record(3,190,++now); // Nonzero feedback must never block stop.
}
void vTaskDelay(unsigned ms) {
    now+=ms*1000;
    if(moving && cancel_move) {
        moving=false; position=4329; feedback();
        MotorRequest stop; requested_stop=motor_core_submit(stop).id;
    }
}
QueueHandle_t xQueueCreate(unsigned count,unsigned bytes) {assert(count==1); item_size=bytes; return reinterpret_cast<void*>(1);}
void vQueueDelete(QueueHandle_t) {}
int xQueueSend(QueueHandle_t,const void *value,unsigned) {
    if(!queued.empty()) return 0;
    queued.resize(item_size); std::memcpy(queued.data(),value,item_size); return 1;
}
int xQueueReceive(QueueHandle_t,void *out,unsigned) {
    if(queued.empty()) throw Idle{};
    std::memcpy(out,queued.data(),item_size); queued.clear(); return 1;
}
void xQueueReset(QueueHandle_t) {queued.clear();}
int xTaskCreate(void (*task)(void*),const char*,unsigned,void*,unsigned,void*) {owner=task; return 1;}
void pump() {try {owner(nullptr);} catch(Idle &) {}}

int ecvt_motor_sdo_read_u8(uint16_t index,uint8_t,uint8_t *out) {
    bus.push_back("read8"); now+=10;
    *out=index==0x1001 ? errors : drive_mode; return read_fails ? -1 : 0;
}
int ecvt_motor_sdo_read_u16(uint16_t index,uint8_t,uint16_t *out) {
    bus.push_back("read16"); now+=10;
    *out=index==0x6041 ? drive_status : 0; return read_fails ? -1 : 0;
}
int ecvt_motor_sdo_read_u32(uint16_t index,uint8_t,uint32_t *out) {
    bus.push_back("read32"); now+=10; ++sample_reads;
    if(index==0x2001) { *out=parameter_value;
        if(cancel_snapshot) { cancel_snapshot=false; MotorRequest stop; requested_stop=motor_core_submit(stop).id; }
    }
    else if(index==0x60ff) *out=target_velocity;
    else if(index==0x6064) *out=static_cast<uint32_t>(position);
    else if(index==0x6062 || index==0x607a) *out=static_cast<uint32_t>(target_position);
    else if(index==0x6079) *out=47900;
    else if(index==0x6091) *out=10000;
    else *out=0;
    return read_fails ? -1 : 0;
}
int ecvt_motor_sdo_write_u8(uint16_t index,uint8_t,uint8_t value) {
    bus.push_back("write8"); if(index==0x6060) drive_mode=value; return 0;
}
int ecvt_motor_sdo_write_u32(uint16_t index,uint8_t,uint32_t value) {
    bus.push_back(index==0x60ff ? "zero" : "write32");
    if(index==0x60ff) {assert(value==0); target_velocity=value;}
    if(index==0x607a) target_position=static_cast<int32_t>(value);
    if(index==0x2001) parameter_value=value;
    return 0;
}
int ecvt_motor_sdo_write_u16(uint16_t index,uint8_t,uint16_t word) {
    if(index!=0x6040) {bus.push_back("write16"); return 0;}
    bus.push_back(word==0 ? "disable" : "control");
    if(word==0) {
        moving=false;
        if(!stuck && !(drive_status&8)) drive_status=0x0250;
        feedback();
        return write_fails ? -1 : 0;
    }
    drive_status=word==6 ? 0x0231 : word==7 ? 0x0233 : 0x0237;
    feedback(); return 0;
}
int ecvt_motor_send_controlword(uint16_t word) {return ecvt_motor_sdo_write_u16(0x6040,0,word);}
int ecvt_motor_nmt_send(uint8_t,uint8_t) {bus.push_back("nmt"); feedback(); return 0;}
int ecvt_motor_send_target_position_and_speed(float revs,float) {
    target_position=std::lround(revs*10000); bus.push_back("target"); return 0;
}
int ecvt_motor_send_target_position_speed_accel_decel(float revs,float,float,float) {
    target_position=std::lround(revs*10000); bus.push_back("target"); return 0;
}
int ecvt_motor_enable_position() {
    if(cancel_move) {moving=true; drive_status=0x0237;}
    else {position=target_position; drive_status=0x0637;}
    feedback(); return 0;
}
float ecvt_motor_get_actual_position_feedback() {return position/10000.0f;}

MotorReceipt submit(MotorCommand kind, float a=0,float b=0,float c=0) {
    MotorRequest request; request.kind=kind; request.values[0]=a; request.values[1]=b; request.values[2]=c;
    return motor_core_submit(request);
}
MotorResult result_for(uint32_t id) {
    for(const auto &result: completed) if(result.id==id) return result;
    assert(false); return {};
}
void stop() {auto receipt=submit(MotorCommand::Stop); assert(receipt.error==0); pump(); assert(result_for(receipt.id).error==0);}

int main() {
    MotorCoreServices services; services.completed=[](void*,const MotorResult &result) {completed.push_back(result);};
    assert(motor_core_start(services)==0 && bus.empty());
    assert(motor_core_start(services)==ESP_ERR_INVALID_STATE);
    assert(!motor_core_status().busy);
    // No frontend, sensor, SD or legacy controller linked. A rebooted owner still stops.
    drive_status=0x0637; drive_mode=4; bus.clear(); stop();
    assert(bus.front()=="disable" && motor_core_status().state==MotorRunState::Idle);
    assert(bus.size()==2); // No mode/velocity read precedes or follows disable.
    auto limits=submit(MotorCommand::Limits,-0.1f,0.6f,10); pump(); assert(result_for(limits.id).error==0);
    auto profile=submit(MotorCommand::Profile,10,10); pump(); assert(result_for(profile.id).error==0);
    auto status=motor_core_status(); assert(status.maximum_rpm==10 && status.acceleration_rpm_s==10);
    // Stop cancels a queued operation before it can execute and coalesces repeated stops.
    bus.clear(); auto queued_move=submit(MotorCommand::Move,0.25,10);
    assert(motor_core_status().active_request_id==queued_move.id);
    assert(submit(MotorCommand::ServoPing).error==ESP_ERR_INVALID_STATE);
    auto first=submit(MotorCommand::Stop); auto second=submit(MotorCommand::ServoControlword,0); assert(first.id==second.id);
    pump(); assert(result_for(queued_move.id).cancelled && result_for(first.id).error==0);
    for(const auto &event:bus) assert(event=="disable" || event=="read16");
    // The actual position runner completes and uses the same unconditional cleanup.
    position=-22; feedback(); auto move=submit(MotorCommand::Move,0.25f,10); pump();
    assert(result_for(move.id).error==0 && !result_for(move.id).cancelled && position==2500);
    assert(motor_core_status().state==MotorRunState::Idle);
    // Cancel while moving; old work must not resume afterward.
    position=4999; feedback(); cancel_move=true;
    move=submit(MotorCommand::Move,0,2); pump(); cancel_move=false;
    assert(result_for(move.id).cancelled && result_for(requested_stop).error==0 && position==4329);
    assert(!motor_core_status().busy && drive_status==0x0250);
    // Mode-3 direct enable writes zero first, then the supervised F state.
    drive_mode=3; drive_status=0x0233; target_velocity=123; bus.clear();
    auto enable=submit(MotorCommand::ServoControlword,15); pump(); assert(result_for(enable.id).error==0);
    auto zero=std::find(bus.begin(),bus.end(),"zero"), control=std::find(bus.begin(),bus.end(),"control");
    assert(zero<control && target_velocity==0);
    auto explicit_disable=submit(MotorCommand::ServoControlword,0); pump();
    assert(result_for(explicit_disable.id).error==0 && drive_status==0x0250);
    // Automatic behavior is optional and must not enable without an adapter.
    auto auto_mode=submit(MotorCommand::ModeAuto); pump();
    submit(MotorCommand::Limits,-6.9f,0,3000); pump(); bus.clear();
    auto automatic=submit(MotorCommand::AutoStart); pump();
    assert(result_for(automatic.id).error==ESP_ERR_INVALID_STATE && bus.empty());
    submit(MotorCommand::ModeManual); pump(); submit(MotorCommand::Limits,-0.1f,0.6f,10); pump();
    // The ten-second brake sequence remains available through the neutral API.
    drive_mode=3; auto brake=submit(MotorCommand::BrakeProbe); int64_t before=now; pump();
    assert(result_for(brake.id).error==0 && now-before>=10000000 && drive_status==0x0250);
    // Full-width parameter values never pass through float; results include widths/times.
    MotorRequest object; object.kind=MotorCommand::ParameterRead; object.object_count=1;
    object.objects[0]={0x2001,0,ObjectWidth::U32,0};
    auto read=motor_core_submit(object); pump(); auto read_result=result_for(read.id);
    assert(read_result.error==0 && read_result.objects[0].object.value==0xfedcba98u);
    object.kind=MotorCommand::ParameterWrite; object.objects[0].value=0xdeadbeefu;
    auto write=motor_core_submit(object); pump(); assert(result_for(write.id).error==0 && parameter_value==0xdeadbeefu);
    object.objects[0].width=ObjectWidth::U8; bus.clear();
    write=motor_core_submit(object); pump(); assert(result_for(write.id).error==ESP_ERR_INVALID_ARG && bus.empty());
    object.objects[0]={0x6040,0,ObjectWidth::U16,15}; bus.clear();
    write=motor_core_submit(object); pump(); assert(result_for(write.id).error==ESP_ERR_INVALID_ARG && bus.empty());
    object.kind=MotorCommand::ParameterRead; object.objects[0]={0x2001,0,ObjectWidth::U32,0};
    read_fails=true; read=motor_core_submit(object); pump(); read_fails=false;
    assert(result_for(read.id).objects[0].error!=0 && result_for(read.id).objects[0].object.value==0);
    object.kind=MotorCommand::ParameterSnapshot; object.object_count=MOTOR_SNAPSHOT_CAPACITY+1;
    assert(motor_core_submit(object).error==ESP_ERR_INVALID_ARG);
    object.object_count=2;
    object.objects[0]={0x2001,0,ObjectWidth::U32,0}; object.objects[1]={0x6061,0,ObjectWidth::U8,0};
    read=motor_core_submit(object); pump(); read_result=result_for(read.id);
    assert(read_result.object_count==2 && read_result.objects[0].sampled_us<read_result.objects[1].sampled_us);
    cancel_snapshot=true; read=motor_core_submit(object); pump(); read_result=result_for(read.id);
    assert(read_result.cancelled && read_result.object_count==1 && result_for(requested_stop).error==0);
    // Stop remains unconditional with auto selected, faults, missing acknowledgements and failed reads.
    submit(MotorCommand::ModeAuto); pump(); drive_status=0x0637; drive_mode=3; bus.clear(); stop();
    assert(bus.front()=="disable" && motor_core_status().automatic);
    drive_status=0x0637; write_fails=true; stop(); write_fails=false;
    read_fails=true; auto failed=submit(MotorCommand::Stop); pump();
    assert(result_for(failed.id).error!=0 && motor_core_status().disable_pending);
    read_fails=false; stop(); assert(!motor_core_status().disable_pending);
    drive_status=0x0637; stuck=true; failed=submit(MotorCommand::Stop); pump();
    assert(result_for(failed.id).error==ESP_ERR_TIMEOUT); stuck=false; stop();
    drive_status=0x1218; failed=submit(MotorCommand::Stop); pump(); assert(result_for(failed.id).error!=0);
    // Read-only parameter diagnostics remain available after failed disable.
    object.kind=MotorCommand::ParameterRead; object.object_count=1; read=motor_core_submit(object); pump();
    assert(result_for(read.id).error==0);
    drive_status=0x0250; stop();
    servo_feedback_record(4,-123,now); assert(motor_core_status().feedback.current_raw==-123);
    puts("Frontend-independent core, parameters, recording-free operation, moves and disable cases passed.");
}
