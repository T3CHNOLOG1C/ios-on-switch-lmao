// ipc_kmsg.c's use of switch_render_senders.h: ipc_kmsg_send counts a message to the watched space under the
// destination port's lock (ip_in_space asserts it), before the message is queued, never for the kernel's own reply.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

using testing::HasSubstr;

namespace {
std::string kmsg()
{
    std::ifstream file(IPC_KMSG, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}
}  // namespace

TEST(RenderSendersSource, CountedUnderThePortLockBeforeQueueing)
{
    const std::string s = kmsg();
    const size_t send = s.find("\nipc_kmsg_send(\n");
    ASSERT_NE(send, std::string::npos);
    const std::string body = s.substr(send, s.find("\n}\n", send) - send);
    const size_t note = body.find("switch_render_sender_note(hdr->msgh_id);");
    ASSERT_NE(note, std::string::npos);
    EXPECT_LT(body.find("ip_mq_lock(port);"), note);
    EXPECT_LT(note, body.find("ipc_mqueue_send_locked(&port->ip_messages, kmsg,"));
    EXPECT_THAT(body,
                HasSubstr("!kernel_reply &&\n\t\t    ip_in_space(port, (ipc_space_t)switch_render_server_space)"));
    // the lock nests inside the port's spin lock: a spin lock too
    EXPECT_THAT(s, HasSubstr("static LCK_SPIN_DECLARE(switch_render_senders_lock"));
}
