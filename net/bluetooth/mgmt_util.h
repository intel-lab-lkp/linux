/* SPDX-License-Identifier: GPL-2.0 */
/*
   BlueZ - Bluetooth protocol stack for Linux
   Copyright (C) 2015  Intel Coropration
*/

struct mgmt_mesh_tx {
	struct list_head list;
	int index;
	size_t param_len;
	struct sock *sk;
	u8 handle;
	u8 instance;
	u8 param[sizeof(struct mgmt_cp_mesh_send) + 31];
};

struct mgmt_pending_cmd {
	struct list_head list;
	u16 opcode;
	struct hci_dev *hdev;
	void *param;
	size_t param_len;
	struct sock *sk;
	struct sk_buff *skb;
	void *user_data;
	int (*cmd_complete)(struct mgmt_pending_cmd *cmd, u8 status);
};

struct sk_buff *mgmt_alloc_skb(struct hci_dev *hdev, u16 opcode,
			       unsigned int size);
int mgmt_send_event_skb(unsigned short channel, struct sk_buff *skb, int flag,
			struct sock *skip_sk);
int mgmt_send_event(u16 event, struct hci_dev *hdev, unsigned short channel,
		    void *data, u16 data_len, int flag, struct sock *skip_sk);
int mgmt_cmd_status(struct sock *sk, u16 index, u16 cmd, u8 status);
int mgmt_cmd_complete(struct sock *sk, u16 index, u16 cmd, u8 status,
		      void *rp, size_t rp_len);

struct mgmt_pending_cmd *mgmt_pending_find(unsigned short channel, u16 opcode,
					   struct hci_dev *hdev);
void mgmt_pending_foreach(u16 opcode, struct hci_dev *hdev, bool remove,
			  void (*cb)(struct mgmt_pending_cmd *cmd, void *data),
			  void *data);
struct mgmt_pending_cmd *mgmt_pending_add(struct sock *sk, u16 opcode,
					  struct hci_dev *hdev,
					  void *data, u16 len);
struct mgmt_pending_cmd *mgmt_pending_new(struct sock *sk, u16 opcode,
					  struct hci_dev *hdev,
					  void *data, u16 len);
void mgmt_pending_free(struct mgmt_pending_cmd *cmd);
void mgmt_pending_remove(struct mgmt_pending_cmd *cmd);
bool __mgmt_pending_listed(struct hci_dev *hdev, struct mgmt_pending_cmd *cmd);
bool mgmt_pending_listed(struct hci_dev *hdev, struct mgmt_pending_cmd *cmd);
bool mgmt_pending_valid(struct hci_dev *hdev, struct mgmt_pending_cmd *cmd);
void mgmt_mesh_foreach(struct hci_dev *hdev,
		       void (*cb)(struct mgmt_mesh_tx *mesh_tx, void *data),
		       void *data, struct sock *sk);
struct mgmt_mesh_tx *mgmt_mesh_find(struct hci_dev *hdev, u8 handle);
struct mgmt_mesh_tx *mgmt_mesh_next(struct hci_dev *hdev, struct sock *sk);
struct mgmt_mesh_tx *mgmt_mesh_add(struct sock *sk, struct hci_dev *hdev,
				   void *data, u16 len);
void mgmt_mesh_remove(struct mgmt_mesh_tx *mesh_tx);
