// SPDX-License-Identifier: GPL-2.0-or-later
/* QUIC kernel implementation
 * (C) Copyright Red Hat Corp. 2023
 *
 * This file is part of the QUIC kernel implementation
 *
 * Initialization/cleanup for QUIC protocol support.
 *
 * Written or modified by:
 *    Xin Long <lucien.xin@gmail.com>
 */

#include "socket.h"
#include "number.h"
#include "frame.h"

/* ACK Frame {
 *  Type (i) = 0x02..0x03,
 *  Largest Acknowledged (i),
 *  ACK Delay (i),
 *  ACK Range Count (i),
 *  First ACK Range (i),
 *  ACK Range (..) ...,
 *  [ECN Counts (..)],
 * }
 */

static struct sk_buff *quic_frame_ack_create(struct sock *sk, void *data, u8 type)
{
	struct quic_gap_ack_block gabs[QUIC_PN_MAX_GABS];
	struct quic_pnmap *map = quic_pnmap(sk);
	u32 largest, smallest, range, pn_ts;
	u32 frame_len, num_gabs;
	struct sk_buff *skb;
	int i;
	u8 *p;

	num_gabs = quic_pnmap_num_gabs(map, gabs);
	frame_len = sizeof(type) + sizeof(u32) * 4;
	frame_len += sizeof(struct quic_gap_ack_block) * num_gabs;

	largest = quic_pnmap_max_pn_seen(map);
	pn_ts = quic_pnmap_max_pn_ts(map);
	smallest = quic_pnmap_min_pn_seen(map);
	if (num_gabs)
		smallest = quic_pnmap_base_pn(map) + gabs[num_gabs - 1].end;
	range = largest - smallest;
	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	pn_ts = jiffies_to_usecs(jiffies) - pn_ts;
	pn_ts = pn_ts / BIT(quic_outq_ack_delay_exponent(quic_outq(sk)));
	p = quic_put_var(skb->data, type);
	p = quic_put_var(p, largest); /* Largest Acknowledged */
	p = quic_put_var(p, pn_ts); /* ACK Delay */
	p = quic_put_var(p, num_gabs); /* ACK Count */
	p = quic_put_var(p, range); /* First ACK Range */

	if (num_gabs) {
		for (i = num_gabs - 1; i > 0; i--) {
			p = quic_put_var(p, gabs[i].end - gabs[i].start); /* Gap */
			p = quic_put_var(p, gabs[i].start - gabs[i - 1].end - 2); /* ACK Range Length */
		}
		p = quic_put_var(p, gabs[0].end - gabs[0].start); /* Gap */
		p = quic_put_var(p, gabs[0].start - 2); /* ACK Range Length */
	}
	frame_len = (u32)(p - skb->data);
	skb_put(skb, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_ping_create(struct sock *sk, void *data, u8 type)
{
	struct sk_buff *skb;
	u8 *p, frame[10];
	u32 frame_len;

	p = quic_put_var(frame, type);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_padding_create(struct sock *sk, void *data, u8 type)
{
	u32 *frame_len = data;
	struct sk_buff *skb;

	skb = alloc_skb(*frame_len + 1, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_zero(skb, *frame_len + 1);
	quic_put_var(skb->data, type);

	return skb;
}

static struct sk_buff *quic_frame_new_token_create(struct sock *sk, void *data, u8 type)
{
	struct quic_token *token = data;
	struct sk_buff *skb;
	u8 *p;

	skb = alloc_skb(token->len + 4, GFP_ATOMIC);
	if (!skb)
		return NULL;
	p = quic_put_var(skb->data, type);
	p = quic_put_var(p, token->len);
	p = quic_put_data(p, token->data, token->len);
	skb_put(skb, (u32)(p - skb->data));

	return skb;
}

static struct sk_buff *quic_frame_stream_create(struct sock *sk, void *data, u8 type)
{
	u32 msg_len, hlen = 1, frame_len, max_frame_len;
	struct quic_msginfo *info = data;
	struct quic_stream *stream;
	struct sk_buff *skb;
	u8 *p;

	max_frame_len = quic_packet_max_payload(quic_packet(sk));
	stream = info->stream;
	hlen += quic_var_len(stream->id);
	if (stream->send.offset) {
		type |= QUIC_STREAM_BIT_OFF;
		hlen += quic_var_len(stream->send.offset);
	}

	type |= QUIC_STREAM_BIT_LEN;
	hlen += quic_var_len(max_frame_len);

	msg_len = iov_iter_count(info->msg);
	if (msg_len <= max_frame_len - hlen) {
		if (info->flag & QUIC_STREAM_FLAG_FIN)
			type |= QUIC_STREAM_BIT_FIN;
	} else {
		msg_len = max_frame_len - hlen;
	}

	skb = alloc_skb(msg_len + hlen, GFP_ATOMIC);
	if (!skb)
		return NULL;

	p = quic_put_var(skb->data, type);
	p = quic_put_var(p, stream->id);
	if (type & QUIC_STREAM_BIT_OFF) {
		p = quic_put_var(p, stream->send.offset);
		QUIC_SND_CB(skb)->stream_offset = stream->send.offset;
	}
	p = quic_put_var(p, msg_len);
	frame_len = (u32)(p - skb->data);

	if (!copy_from_iter_full(p, msg_len, info->msg)) {
		kfree_skb(skb);
		return NULL;
	}
	frame_len += msg_len;
	skb_put(skb, frame_len);
	QUIC_SND_CB(skb)->data_bytes = msg_len;
	QUIC_SND_CB(skb)->stream = stream;
	QUIC_SND_CB(skb)->frame_type = type;

	stream->send.offset += msg_len;
	return skb;
}

static struct sk_buff *quic_frame_handshake_done_create(struct sock *sk, void *data, u8 type)
{
	struct sk_buff *skb;
	u8 *p, frame[10];
	u32 frame_len;

	p = quic_put_var(frame, type);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_crypto_create(struct sock *sk, void *data, u8 type)
{
	struct quic_token *ticket = data;
	struct sk_buff *skb;
	u8 *p;

	skb = alloc_skb(ticket->len + 8, GFP_ATOMIC);
	if (!skb)
		return NULL;
	p = quic_put_var(skb->data, type);
	p = quic_put_var(p, 0);
	p = quic_put_var(p, ticket->len);
	p = quic_put_data(p, ticket->data, ticket->len);
	skb_put(skb, (u32)(p - skb->data));

	return skb;
}

static struct sk_buff *quic_frame_retire_connection_id_create(struct sock *sk, void *data, u8 type)
{
	struct sk_buff *skb;
	u64 *number = data;
	u8 *p, frame[10];
	u32 frame_len;

	p = quic_put_var(frame, type);
	p = quic_put_var(p, *number);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	quic_connection_id_remove(quic_dest(sk), *number);
	return skb;
}

static struct sk_buff *quic_frame_new_connection_id_create(struct sock *sk, void *data, u8 type)
{
	u8 *p, frame[100], conn_id[16], token[16];
	u64 *prior = data, seqno;
	struct sk_buff *skb;
	u32 frame_len;
	int err;

	seqno = quic_connection_id_last_number(quic_source(sk)) + 1;

	p = quic_put_var(frame, type);
	p = quic_put_var(p, seqno);
	p = quic_put_var(p, *prior);
	p = quic_put_var(p, 16);
	get_random_bytes(conn_id, 16);
	p = quic_put_data(p, conn_id, 16);
	p = quic_put_data(p, token, 16);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	err = quic_connection_id_append(quic_source(sk), seqno, sk, conn_id, 16);
	if (err) {
		kfree_skb(skb);
		return NULL;
	}

	return skb;
}

static struct sk_buff *quic_frame_path_response_create(struct sock *sk, void *data, u8 type)
{
	u8 *p, frame[10], *entropy = data;
	struct sk_buff *skb;
	u32 frame_len;

	p = quic_put_var(frame, type);
	p = quic_put_data(p, entropy, 8);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_path_challenge_create(struct sock *sk, void *data, u8 type)
{
	struct quic_path_addr *path = data;
	struct sk_buff *skb;
	u8 *p, frame[10];
	u32 frame_len;

	get_random_bytes(path->entropy, sizeof(path->entropy));

	p = quic_put_var(frame, type);
	p = quic_put_data(p, path->entropy, sizeof(path->entropy));
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_reset_stream_create(struct sock *sk, void *data, u8 type)
{
	struct quic_stream_table *streams = quic_streams(sk);
	struct quic_errinfo *info = data;
	struct quic_stream *stream;
	struct sk_buff *skb;
	u8 *p, frame[20];
	u32 frame_len;

	stream = quic_stream_find(streams, info->stream_id);
	if (!stream)
		return NULL;

	p = quic_put_var(frame, type);
	p = quic_put_var(p, info->stream_id);
	p = quic_put_var(p, info->errcode);
	p = quic_put_var(p, stream->send.offset);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);
	QUIC_SND_CB(skb)->err_code = info->errcode;
	QUIC_SND_CB(skb)->stream = stream;

	if (streams->send.stream_active == stream->id)
		streams->send.stream_active = -1;

	return skb;
}

static struct sk_buff *quic_frame_stop_sending_create(struct sock *sk, void *data, u8 type)
{
	struct quic_errinfo *info = data;
	struct sk_buff *skb;
	u8 *p, frame[10];
	u32 frame_len;

	p = quic_put_var(frame, type);
	p = quic_put_var(p, info->stream_id);
	p = quic_put_var(p, info->errcode);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_max_data_create(struct sock *sk, void *data, u8 type)
{
	struct quic_inqueue *inq = data;
	struct sk_buff *skb;
	u8 *p, frame[10];
	u32 frame_len;

	p = quic_put_var(frame, type);
	p = quic_put_var(p, inq->max_bytes);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_max_stream_data_create(struct sock *sk, void *data, u8 type)
{
	struct quic_stream *stream = data;
	struct sk_buff *skb;
	u8 *p, frame[10];
	u32 frame_len;

	p = quic_put_var(frame, type);
	p = quic_put_var(p, stream->id);
	p = quic_put_var(p, stream->recv.max_bytes);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_max_streams_uni_create(struct sock *sk, void *data, u8 type)
{
	struct sk_buff *skb;
	u8 *p, frame[10];
	u64 *max = data;
	int frame_len;

	p = quic_put_var(frame, type);
	p = quic_put_var(p, *max);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_max_streams_bidi_create(struct sock *sk, void *data, u8 type)
{
	struct sk_buff *skb;
	u8 *p, frame[10];
	u64 *max = data;
	int frame_len;

	p = quic_put_var(frame, type);
	p = quic_put_var(p, *max);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_connection_close_create(struct sock *sk, void *data, u8 type)
{
	u32 frame_len, phrase_len = 0;
	struct sk_buff *skb;
	u8 *p, frame[100];

	p = quic_put_var(frame, type);
	p = quic_put_var(p, quic_outq(sk)->close_errcode);

	if (type == QUIC_FRAME_CONNECTION_CLOSE)
		p = quic_put_var(p, quic_outq(sk)->close_frame);

	if (quic_outq(sk)->close_phrase)
		phrase_len = strlen(quic_outq(sk)->close_phrase) + 1;
	p = quic_put_var(p, phrase_len);
	p = quic_put_data(p, quic_outq(sk)->close_phrase, phrase_len);

	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_data_blocked_create(struct sock *sk, void *data, u8 type)
{
	struct quic_outqueue *outq = data;
	struct sk_buff *skb;
	u8 *p, frame[10];
	u32 frame_len;

	p = quic_put_var(frame, type);
	p = quic_put_var(p, outq->max_bytes);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_stream_data_blocked_create(struct sock *sk, void *data, u8 type)
{
	struct quic_stream *stream = data;
	struct sk_buff *skb;
	u8 *p, frame[10];
	u32 frame_len;

	p = quic_put_var(frame, type);
	p = quic_put_var(p, stream->id);
	p = quic_put_var(p, stream->send.max_bytes);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_streams_blocked_uni_create(struct sock *sk, void *data, u8 type)
{
	u32 *max = data, frame_len;
	struct sk_buff *skb;
	u8 *p, frame[10];

	p = quic_put_var(frame, type);
	p = quic_put_var(p, (*max >> 2) + 1);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static struct sk_buff *quic_frame_streams_blocked_bidi_create(struct sock *sk, void *data, u8 type)
{
	u32 *max = data, frame_len;
	struct sk_buff *skb;
	u8 *p, frame[10];

	p = quic_put_var(frame, type);
	p = quic_put_var(p, (*max >> 2) + 1);
	frame_len = (u32)(p - frame);

	skb = alloc_skb(frame_len, GFP_ATOMIC);
	if (!skb)
		return NULL;
	skb_put_data(skb, frame, frame_len);

	return skb;
}

static int quic_frame_crypto_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_token *ticket = quic_ticket(sk);
	u64 offset, length;
	u32 len = skb->len;
	u8 *p = skb->data;

	if (!quic_get_var(&p, &len, &offset) || offset)
		return -EINVAL;
	if (!quic_get_var(&p, &len, &length) || length > len)
		return -EINVAL;
	if (*p != 4) /* for TLS NEWSESSION_TICKET message only */
		return -EINVAL;

	p = kmemdup(p, length, GFP_ATOMIC);
	if (!p)
		return -ENOMEM;
	kfree(ticket->data);
	ticket->data = p;
	ticket->len = length;

	len -= length;
	return skb->len - len;
}

static int quic_frame_stream_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	u64 stream_id, payload_len, offset = 0;
	struct quic_stream *stream;
	struct sk_buff *nskb;
	u32 len = skb->len;
	u8 *p = skb->data;
	int err;

	if (!quic_get_var(&p, &len, &stream_id))
		return -EINVAL;
	if (type & QUIC_STREAM_BIT_OFF) {
		if (!quic_get_var(&p, &len, &offset))
			return -EINVAL;
	}

	payload_len = len;
	if (type & QUIC_STREAM_BIT_LEN) {
		if (!quic_get_var(&p, &len, &payload_len) || payload_len > len)
			return -EINVAL;
	}

	stream = quic_stream_recv_get(quic_streams(sk), stream_id, quic_is_serv(sk));
	if (!stream)
		return -EINVAL;

	nskb = skb_clone(skb, GFP_ATOMIC);
	if (!nskb)
		return -ENOMEM;
	skb_pull(nskb, skb->len - len);
	skb_trim(nskb, payload_len);

	QUIC_RCV_CB(nskb)->stream = stream;
	QUIC_RCV_CB(nskb)->stream_fin = (type & QUIC_STREAM_BIT_FIN);
	QUIC_RCV_CB(nskb)->stream_offset = offset;

	err = quic_inq_reasm_tail(sk, nskb);
	if (err) {
		kfree_skb(nskb);
		return err;
	}

	len -= payload_len;
	return skb->len - len;
}

static int quic_frame_ack_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	u64 largest, smallest, range, delay, count, gap, i;
	u32 len = skb->len;
	u8 *p = skb->data;

	if (!quic_get_var(&p, &len, &largest) ||
	    !quic_get_var(&p, &len, &delay) ||
	    !quic_get_var(&p, &len, &count) || count > 16 ||
	    !quic_get_var(&p, &len, &range))
		return -EINVAL;

	smallest = largest - range;
	quic_outq_retransmit_check(sk, largest, smallest, largest, delay);

	for (i = 0; i < count; i++) {
		if (!quic_get_var(&p, &len, &gap) ||
		    !quic_get_var(&p, &len, &range))
			return -EINVAL;
		largest = smallest - gap - 2;
		smallest = largest - range;
		quic_outq_retransmit_check(sk, largest, smallest, 0, 0);
	}

	if (type == QUIC_FRAME_ACK_ECN) { /* TODO */
		if (!quic_get_var(&p, &len, &count) ||
		    !quic_get_var(&p, &len, &count) ||
		    !quic_get_var(&p, &len, &count))
			return -EINVAL;
	}

	return skb->len - len;
}

static int quic_frame_new_connection_id_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_connection_id_set *id_set = quic_dest(sk);
	u8 *p = skb->data, *conn_id, *token;
	u64 seqno, prior, length, first;
	struct sk_buff *nskb;
	u32 len = skb->len;
	int err;

	if (!quic_get_var(&p, &len, &seqno) ||
	    !quic_get_var(&p, &len, &prior) ||
	    !quic_get_var(&p, &len, &length) || length + 16 > len)
		return -EINVAL;

	conn_id = p;
	token = conn_id + length; /* TODO: Stateless Reset */

	if (seqno != quic_connection_id_last_number(id_set) + 1 || prior > seqno)
		return -EINVAL;

	err = quic_connection_id_append(id_set, seqno, sk, conn_id, length);
	if (err)
		return err;

	first = quic_connection_id_first_number(id_set);
	if (prior <= first)
		goto out;

	for (; first < prior; first++) {
		nskb = quic_frame_create(sk, QUIC_FRAME_RETIRE_CONNECTION_ID, &first);
		if (!nskb)
			return -ENOMEM;
		quic_outq_ctrl_tail(sk, nskb, true);
	}

out:
	len -= (length + 16);
	return skb->len - len;
}

static int quic_frame_retire_connection_id_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_connection_id_set *id_set = quic_source(sk);
	u32 len = skb->len, last, first;
	struct sk_buff *nskb;
	u8 *p = skb->data;
	u64 seqno;

	if (!quic_get_var(&p, &len, &seqno))
		return -EINVAL;
	last  = quic_connection_id_last_number(id_set);
	first = quic_connection_id_first_number(id_set);
	if (seqno != first || seqno == last)
		return -EINVAL;

	quic_connection_id_remove(id_set, seqno);
	if (last - seqno >= quic_source(sk)->max_count)
		goto out;
	seqno++;
	nskb = quic_frame_create(sk, QUIC_FRAME_NEW_CONNECTION_ID, &seqno);
	if (!nskb)
		return -ENOMEM;
	quic_outq_ctrl_tail(sk, nskb, true);
out:
	return skb->len - len;
}

static int quic_frame_new_token_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_token *token = quic_token(sk);
	u32 len = skb->len;
	u8 *p = skb->data;
	u64 length;

	if (!quic_get_var(&p, &len, &length) || length > len)
		return -EINVAL;
	p = kmemdup(p, length, GFP_ATOMIC);
	if (!p)
		return -ENOMEM;
	kfree(token->data);
	token->data = p;
	token->len = length;

	len -= length;
	return skb->len - len;
}

static int quic_frame_handshake_done_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	return 0; /* no content */
}

static int quic_frame_padding_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	return skb->len;
}

static int quic_frame_ping_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	return 0; /* no content */
}

static int quic_frame_path_challenge_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct sk_buff *fskb;
	u32 len = skb->len;
	u8 entropy[8];

	if (len < 8)
		return -EINVAL;
	memcpy(entropy, skb->data, 8);
	fskb = quic_frame_create(sk, QUIC_FRAME_PATH_RESPONSE, entropy);
	if (!fskb)
		return -ENOMEM;
	quic_outq_ctrl_tail(sk, fskb, true);

	len -= 8;
	return skb->len - len;
}

static int quic_frame_reset_stream_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	u64 stream_id, errcode, finalsz;
	struct quic_stream *stream;
	u32 len = skb->len;
	u8 *p = skb->data;

	if (!quic_get_var(&p, &len, &stream_id) ||
	    !quic_get_var(&p, &len, &errcode) ||
	    !quic_get_var(&p, &len, &finalsz))
		return -EINVAL;

	stream = quic_stream_recv_get(quic_streams(sk), stream_id, quic_is_serv(sk));
	if (IS_ERR(stream))
		return PTR_ERR(stream);

	stream->recv.state = QUIC_STREAM_RECV_STATE_RESET_RECVD;
	return skb->len - len;
}

static int quic_frame_stop_sending_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_stream *stream;
	struct quic_errinfo info;
	u64 stream_id, errcode;
	struct sk_buff *nskb;
	u32 len = skb->len;
	u8 *p = skb->data;

	if (!quic_get_var(&p, &len, &stream_id) ||
	    !quic_get_var(&p, &len, &errcode))
		return -EINVAL;

	stream = quic_stream_send_get(quic_streams(sk), stream_id, 0, quic_is_serv(sk));
	if (IS_ERR(stream))
		return PTR_ERR(stream);

	info.stream_id = stream_id;
	info.errcode = errcode;
	nskb = quic_frame_create(sk, QUIC_FRAME_RESET_STREAM, &info);
	if (!nskb)
		return -ENOMEM;

	stream->send.state = QUIC_STREAM_SEND_STATE_RESET_SENT;
	quic_outq_ctrl_tail(sk, nskb, true);
	return skb->len - len;
}

static int quic_frame_max_data_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_outqueue *outq = quic_outq(sk);
	u32 len = skb->len;
	u8 *p = skb->data;
	u64 max_bytes;

	if (!quic_get_var(&p, &len, &max_bytes))
		return -EINVAL;

	if (max_bytes >= outq->max_bytes) {
		outq->max_bytes = max_bytes;
		outq->data_blocked = 0;
	}

	return skb->len - len;
}

static int quic_frame_max_stream_data_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_stream *stream;
	u64 max_bytes, stream_id;
	u32 len = skb->len;
	u8 *p = skb->data;

	if (!quic_get_var(&p, &len, &stream_id) ||
	    !quic_get_var(&p, &len, &max_bytes))
		return -EINVAL;

	stream = quic_stream_find(quic_streams(sk), stream_id);
	if (!stream)
		return -EINVAL;

	if (max_bytes >= stream->send.max_bytes) {
		stream->send.max_bytes = max_bytes;
		stream->send.data_blocked = 0;
	}

	return skb->len - len;
}

static int quic_frame_max_streams_uni_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_stream_table *streams = quic_streams(sk);
	u32 len = skb->len;
	u64 max, stream_id;
	u8 *p = skb->data;

	if (!quic_get_var(&p, &len, &max))
		return -EINVAL;

	if (max < streams->send.max_streams_uni)
		goto out;

	stream_id = ((max - 1) << 2) | QUIC_STREAM_TYPE_UNI_MASK;
	if (quic_is_serv(sk))
		stream_id |= QUIC_STREAM_TYPE_SERVER_MASK;
	streams->send.max_streams_uni = max;
	streams->send.streams_uni = max;
	sk->sk_write_space(sk);
out:
	return skb->len - len;
}

static int quic_frame_max_streams_bidi_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_stream_table *streams = quic_streams(sk);
	u32 len = skb->len;
	u64 max, stream_id;
	u8 *p = skb->data;

	if (!quic_get_var(&p, &len, &max))
		return -EINVAL;

	if (max < streams->send.max_streams_bidi)
		goto out;

	stream_id = ((max - 1) << 2);
	if (quic_is_serv(sk))
		stream_id |= QUIC_STREAM_TYPE_SERVER_MASK;
	streams->send.max_streams_bidi = max;
	streams->send.streams_bidi = max;
	sk->sk_write_space(sk);
out:
	return skb->len - len;
}

static int quic_frame_connection_close_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_connection_close *close;
	u64 err_code, phrase_len, ftype = 0;
	u8 *p = skb->data, frame[100] = {};
	u32 len = skb->len;

	if (!quic_get_var(&p, &len, &err_code))
		return -EINVAL;
	if (type == QUIC_FRAME_CONNECTION_CLOSE &&
	    !quic_get_var(&p, &len, &ftype))
		return -EINVAL;

	if (!quic_get_var(&p, &len, &phrase_len) || phrase_len > len)
		return -EINVAL;

	close = (void *)frame;
	if (phrase_len) {
		if ((phrase_len > 80 || *(p + phrase_len - 1) != 0))
			return -EINVAL;
		strcpy(close->phrase, p);
	}

	sk->sk_err = -EPIPE;
	quic_set_state(sk, QUIC_STATE_USER_CLOSED);

	/*
	 * Now that state is QUIC_STATE_USER_CLOSED, we can wake the waiting
	 * recv thread up.
	 */
	sk->sk_state_change(sk);

	len -= phrase_len;
	return skb->len - len;
}

static int quic_frame_data_blocked_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_inqueue *inq = quic_inq(sk);
	u64 max_bytes, recv_max_bytes;
	struct sk_buff *fskb;
	u32 len = skb->len;
	u8 *p = skb->data;

	if (!quic_get_var(&p, &len, &max_bytes))
		return -EINVAL;
	recv_max_bytes = inq->max_bytes;

	inq->max_bytes = inq->bytes + inq->window;
	fskb = quic_frame_create(sk, QUIC_FRAME_MAX_DATA, inq);
	if (!fskb) {
		inq->max_bytes = recv_max_bytes;
		return -ENOMEM;
	}
	quic_outq_ctrl_tail(sk, fskb, true);
	return skb->len - len;
}

static int quic_frame_stream_data_blocked_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	u64 stream_id, max_bytes, recv_max_bytes;
	struct quic_stream *stream;
	struct sk_buff *fskb;
	u32 len = skb->len;
	u8 *p = skb->data;

	if (!quic_get_var(&p, &len, &stream_id) ||
	    !quic_get_var(&p, &len, &max_bytes))
		return -EINVAL;

	stream = quic_stream_find(quic_streams(sk), stream_id);
	if (!stream)
		return -EINVAL;

	recv_max_bytes = stream->recv.max_bytes;
	stream->recv.max_bytes = stream->recv.bytes + stream->recv.window;
	if (recv_max_bytes != stream->recv.max_bytes) {
		fskb = quic_frame_create(sk, QUIC_FRAME_MAX_STREAM_DATA, stream);
		if (!fskb) {
			stream->recv.max_bytes = recv_max_bytes;
			return -ENOMEM;
		}
		quic_outq_ctrl_tail(sk, fskb, true);
	}
	return skb->len - len;
}

static int quic_frame_streams_blocked_uni_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct sk_buff *fskb;
	u32 len = skb->len;
	u8 *p = skb->data;
	u64 max;

	if (!quic_get_var(&p, &len, &max))
		return -EINVAL;
	if (max < quic_streams(sk)->recv.max_streams_uni)
		goto out;
	fskb = quic_frame_create(sk, QUIC_FRAME_MAX_STREAMS_UNI, &max);
	if (!fskb)
		return -ENOMEM;
	quic_outq_ctrl_tail(sk, fskb, true);
	quic_streams(sk)->recv.max_streams_uni = max;
out:
	return skb->len - len;
}

static int quic_frame_streams_blocked_bidi_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct sk_buff *fskb;
	u32 len = skb->len;
	u8 *p = skb->data;
	u64 max;

	if (!quic_get_var(&p, &len, &max))
		return -EINVAL;
	if (max < quic_streams(sk)->recv.max_streams_bidi)
		goto out;
	fskb = quic_frame_create(sk, QUIC_FRAME_MAX_STREAMS_BIDI, &max);
	if (!fskb)
		return -ENOMEM;
	quic_outq_ctrl_tail(sk, fskb, true);
	quic_streams(sk)->recv.max_streams_bidi = max;
out:
	return skb->len - len;
}

static int quic_frame_path_response_process(struct sock *sk, struct sk_buff *skb, u8 type)
{
	struct quic_sock *qs = quic_sk(sk);
	struct quic_path_addr *path;
	u8 entropy[8], local = 1;
	u32 len = skb->len;

	if (len < 8)
		return -EINVAL;
	memcpy(entropy, skb->data, 8);

	path = &qs->src; /* source address validation */
	if (!memcmp(path->entropy, entropy, 8)) {
		if (path->pending) {
			path->pending = 0;
			quic_udp_sock_put(qs->udp_sk[!path->active]);
			qs->udp_sk[!path->active] = NULL;
			memset(&path->addr[!path->active], 0, quic_addr_len(sk));
			quic_set_sk_addr(sk, &path->addr[path->active], true);
		}
	}
	path = &qs->dst; /* dest address validation */
	if (!memcmp(path->entropy, entropy, 8)) {
		if (path->pending) {
			local = 0;
			path->pending = 0;
			memset(&path->addr[!path->active], 0, quic_addr_len(sk));
			quic_set_sk_addr(sk, &path->addr[path->active], false);
		}
	}
	len -= 8;
	return skb->len - len;
}

#define quic_frame_create_and_process(type) \
	{quic_frame_##type##_create, quic_frame_##type##_process}

static struct quic_frame_ops quic_frame_ops[QUIC_FRAME_BASE_MAX + 1] = {
	quic_frame_create_and_process(padding), /* 0x00 */
	quic_frame_create_and_process(ping),
	quic_frame_create_and_process(ack),
	quic_frame_create_and_process(ack), /* ack_ecn */
	quic_frame_create_and_process(reset_stream),
	quic_frame_create_and_process(stop_sending),
	quic_frame_create_and_process(crypto),
	quic_frame_create_and_process(new_token),
	quic_frame_create_and_process(stream),
	quic_frame_create_and_process(stream),
	quic_frame_create_and_process(stream),
	quic_frame_create_and_process(stream),
	quic_frame_create_and_process(stream),
	quic_frame_create_and_process(stream),
	quic_frame_create_and_process(stream),
	quic_frame_create_and_process(stream),
	quic_frame_create_and_process(max_data), /* 0x10 */
	quic_frame_create_and_process(max_stream_data),
	quic_frame_create_and_process(max_streams_bidi),
	quic_frame_create_and_process(max_streams_uni),
	quic_frame_create_and_process(data_blocked),
	quic_frame_create_and_process(stream_data_blocked),
	quic_frame_create_and_process(streams_blocked_bidi),
	quic_frame_create_and_process(streams_blocked_uni),
	quic_frame_create_and_process(new_connection_id),
	quic_frame_create_and_process(retire_connection_id),
	quic_frame_create_and_process(path_challenge),
	quic_frame_create_and_process(path_response),
	quic_frame_create_and_process(connection_close),
	quic_frame_create_and_process(connection_close),
	quic_frame_create_and_process(handshake_done),
};

int quic_frame_process(struct sock *sk, struct sk_buff *skb, struct quic_packet_info *pki)
{
	int ret;
	u8 type;

	if (!skb->len)
		return -EINVAL;

	while (1) {
		type = *(u8 *)(skb->data);
		skb_pull(skb, 1);

		if (type > QUIC_FRAME_BASE_MAX) {
			pr_err_once("[QUIC] frame err: unsupported frame %x\n", type);
			return -EPROTONOSUPPORT;
		}
		pr_debug("[QUIC] frame process %x\n", type);
		ret = quic_frame_ops[type].frame_process(sk, skb, type);
		if (ret < 0) {
			pr_warn("[QUIC] frame err %x %d\n", type, ret);
			return ret;
		}
		if (quic_frame_ack_eliciting(type)) {
			pki->ack_eliciting = 1;
			if (quic_frame_ack_immediate(type))
				pki->ack_immediate = 1;
		}
		if (quic_frame_non_probing(type))
			pki->non_probing = 1;

		skb_pull(skb, ret);
		if (skb->len <= 0)
			break;
	}
	return 0;
}

struct sk_buff *quic_frame_create(struct sock *sk, u8 type, void *data)
{
	struct sk_buff *skb;

	if (type > QUIC_FRAME_BASE_MAX)
		return NULL;
	pr_debug("[QUIC] frame create %u\n", type);
	skb = quic_frame_ops[type].frame_create(sk, data, type);
	if (!skb) {
		pr_err("[QUIC] frame create failed %x\n", type);
		return NULL;
	}
	if (!QUIC_SND_CB(skb)->frame_type)
		QUIC_SND_CB(skb)->frame_type = type;
	return skb;
}
