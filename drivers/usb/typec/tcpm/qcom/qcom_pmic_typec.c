// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023, Linaro Ltd. All rights reserved.
 */

#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/usb/role.h>
#include <linux/usb/tcpm.h>
#include <linux/usb/typec_mux.h>
#include "qcom_pmic_typec_pdphy.h"
#include "qcom_pmic_typec_port.h"

struct pmic_typec_resources {
	struct pmic_typec_pdphy_resources	*pdphy_res;
	struct pmic_typec_port_resources	*port_res;
};

struct pmic_typec {
	struct device		*dev;
	struct tcpm_port	*tcpm_port;
	struct tcpc_dev		tcpc;
	struct pmic_typec_pdphy	*pmic_typec_pdphy;
	struct pmic_typec_port	*pmic_typec_port;
	bool			vbus_enabled;
	struct mutex		lock;		/* VBUS state serialization */
};

#define tcpc_to_tcpm(_tcpc_) container_of(_tcpc_, struct pmic_typec, tcpc)

static int qcom_pmic_typec_get_vbus(struct tcpc_dev *tcpc)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);
	int ret;

	mutex_lock(&tcpm->lock);
	ret = tcpm->vbus_enabled || qcom_pmic_typec_port_get_vbus(tcpm->pmic_typec_port);
	mutex_unlock(&tcpm->lock);

	return ret;
}

static int qcom_pmic_typec_set_vbus(struct tcpc_dev *tcpc, bool on, bool sink)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);
	int ret = 0;

	mutex_lock(&tcpm->lock);
	if (tcpm->vbus_enabled == on)
		goto done;

	ret = qcom_pmic_typec_port_set_vbus(tcpm->pmic_typec_port, on);
	if (ret)
		goto done;

	tcpm->vbus_enabled = on;
	tcpm_vbus_change(tcpm->tcpm_port);

done:
	dev_info(tcpm->dev, "set_vbus set: %d result %d\n", on, ret);
	mutex_unlock(&tcpm->lock);

	return ret;
}

static int qcom_pmic_typec_set_current_limit(struct tcpc_dev *tcpc, u32 ma, u32 mv)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);
	if (!mv)
		return 0;

	if (mv > 0 && mv != 5000) {
		dev_info(tcpm->dev, "set_current_limit: unsupported voltage %d\n",
			 mv);
	}

	dev_info(tcpm->dev, "set_current_limit: %d mA\n", ma);

	return qcom_pmic_typec_port_set_vbus_current_limit(tcpm->pmic_typec_port, ma);
}

static int qcom_pmic_typec_set_vconn(struct tcpc_dev *tcpc, bool on)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);

	return qcom_pmic_typec_port_set_vconn(tcpm->pmic_typec_port, on);
}

static int qcom_pmic_typec_get_cc(struct tcpc_dev *tcpc,
				  enum typec_cc_status *cc1,
				  enum typec_cc_status *cc2)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);

	return qcom_pmic_typec_port_get_cc(tcpm->pmic_typec_port, cc1, cc2);
}

static bool qcom_pmic_typec_is_vbus_vsafe0v(struct tcpc_dev *tcpc)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);

	return qcom_pmic_typec_port_is_vbus_vsafe0v(tcpm->pmic_typec_port);
}

static int qcom_pmic_typec_set_cc(struct tcpc_dev *tcpc,
				  enum typec_cc_status cc)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);

	return qcom_pmic_typec_port_set_cc(tcpm->pmic_typec_port, cc);
}

static int qcom_pmic_typec_set_polarity(struct tcpc_dev *tcpc,
					enum typec_cc_polarity pol)
{
	/* Polarity is set separately by phy-qcom-qmp.c */
	return 0;
}

static int qcom_pmic_typec_start_toggling(struct tcpc_dev *tcpc,
					  enum typec_port_type port_type,
					  enum typec_cc_status cc)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);

	return qcom_pmic_typec_port_start_toggling(tcpm->pmic_typec_port,
						   port_type, cc);
}

static int qcom_pmic_typec_set_roles(struct tcpc_dev *tcpc, bool attached,
				     enum typec_role power_role,
				     enum typec_data_role data_role)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);

	return qcom_pmic_typec_pdphy_set_roles(tcpm->pmic_typec_pdphy,
					       data_role, power_role);
}

static int qcom_pmic_typec_set_pd_rx(struct tcpc_dev *tcpc, bool on)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);

	/* If we're in a SNK with a legacy cable connected then we will never receive
	 * anything from PD. Avoid the whole song and dance and return an error
	 * in that case so that the state machine will jump straight to SNK_READY
	 */
	if (qcom_pmic_typec_port_is_legacy_cable(tcpm->pmic_typec_port))
		return -EINVAL;

	return qcom_pmic_typec_pdphy_set_pd_rx(tcpm->pmic_typec_pdphy, on);
}

static int qcom_pmic_typec_pd_transmit(struct tcpc_dev *tcpc,
				       enum tcpm_transmit_type type,
				       const struct pd_message *msg,
				       unsigned int negotiated_rev)
{
	struct pmic_typec *tcpm = tcpc_to_tcpm(tcpc);

	return qcom_pmic_typec_pdphy_pd_transmit(tcpm->pmic_typec_pdphy, type,
						 msg, negotiated_rev);
}

static int qcom_pmic_typec_init(struct tcpc_dev *tcpc)
{
	return 0;
}

static int qcom_pmic_typec_probe(struct platform_device *pdev)
{
	struct pmic_typec *tcpm;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct pmic_typec_resources *res;
	struct regmap *regmap;
	u32 base[2];
	int ret;

	res = of_device_get_match_data(dev);
	if (!res)
		return -ENODEV;

	tcpm = devm_kzalloc(dev, sizeof(*tcpm), GFP_KERNEL);
	if (!tcpm)
		return -ENOMEM;

	tcpm->dev = dev;
	tcpm->tcpc.init = qcom_pmic_typec_init;
	tcpm->tcpc.get_vbus = qcom_pmic_typec_get_vbus;
	tcpm->tcpc.set_vbus = qcom_pmic_typec_set_vbus;
	tcpm->tcpc.set_cc = qcom_pmic_typec_set_cc;
	tcpm->tcpc.get_cc = qcom_pmic_typec_get_cc;
	tcpm->tcpc.set_polarity = qcom_pmic_typec_set_polarity;
	tcpm->tcpc.set_vconn = qcom_pmic_typec_set_vconn;
	tcpm->tcpc.start_toggling = qcom_pmic_typec_start_toggling;
	tcpm->tcpc.set_pd_rx = qcom_pmic_typec_set_pd_rx;
	tcpm->tcpc.set_roles = qcom_pmic_typec_set_roles;
	tcpm->tcpc.pd_transmit = qcom_pmic_typec_pd_transmit;
	tcpm->tcpc.is_vbus_vsafe0v = qcom_pmic_typec_is_vbus_vsafe0v;
	tcpm->tcpc.set_current_limit = qcom_pmic_typec_set_current_limit;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap) {
		dev_err(dev, "Failed to get regmap\n");
		return -ENODEV;
	}

	ret = of_property_read_u32_array(np, "reg", base, 2);
	if (ret)
		return ret;

	tcpm->pmic_typec_port = qcom_pmic_typec_port_alloc(dev);
	if (IS_ERR(tcpm->pmic_typec_port))
		return PTR_ERR(tcpm->pmic_typec_port);

	tcpm->pmic_typec_pdphy = qcom_pmic_typec_pdphy_alloc(dev);
	if (IS_ERR(tcpm->pmic_typec_pdphy))
		return PTR_ERR(tcpm->pmic_typec_pdphy);

	ret = qcom_pmic_typec_port_probe(pdev, tcpm->pmic_typec_port,
					 res->port_res, regmap, base[0]);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to probe port\n");

	ret = qcom_pmic_typec_pdphy_probe(pdev, tcpm->pmic_typec_pdphy,
					  res->pdphy_res, regmap, base[1]);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to probe pdphy\n");

	mutex_init(&tcpm->lock);
	platform_set_drvdata(pdev, tcpm);

	tcpm->tcpc.fwnode = device_get_named_child_node(tcpm->dev, "connector");
	if (IS_ERR(tcpm->tcpc.fwnode))
		return PTR_ERR(tcpm->tcpc.fwnode);

	tcpm->tcpm_port = tcpm_register_port(tcpm->dev, &tcpm->tcpc);
	if (IS_ERR(tcpm->tcpm_port)) {
		ret = dev_err_probe(dev, PTR_ERR(tcpm->tcpm_port), "Failed to register TCPM port\n");
		goto fwnode_remove;
	}

	ret = qcom_pmic_typec_port_start(tcpm->pmic_typec_port,
					 tcpm->tcpm_port);
	if (ret)
		goto fwnode_remove;

	ret = qcom_pmic_typec_pdphy_start(tcpm->pmic_typec_pdphy,
					  tcpm->tcpm_port);
	if (ret)
		goto fwnode_remove;

	return 0;

fwnode_remove:
	fwnode_remove_software_node(tcpm->tcpc.fwnode);

	return ret;
}

static void qcom_pmic_typec_remove(struct platform_device *pdev)
{
	struct pmic_typec *tcpm = platform_get_drvdata(pdev);

	qcom_pmic_typec_pdphy_stop(tcpm->pmic_typec_pdphy);
	qcom_pmic_typec_port_stop(tcpm->pmic_typec_port);
	tcpm_unregister_port(tcpm->tcpm_port);
	fwnode_remove_software_node(tcpm->tcpc.fwnode);
}

static struct pmic_typec_pdphy_resources pm8150b_pdphy_res = {
	.irq_params = {
		{
			.virq = PMIC_PDPHY_SIG_TX_IRQ,
			.irq_name = "sig-tx",
		},
		{
			.virq = PMIC_PDPHY_SIG_RX_IRQ,
			.irq_name = "sig-rx",
		},
		{
			.virq = PMIC_PDPHY_MSG_TX_IRQ,
			.irq_name = "msg-tx",
		},
		{
			.virq = PMIC_PDPHY_MSG_RX_IRQ,
			.irq_name = "msg-rx",
		},
		{
			.virq = PMIC_PDPHY_MSG_TX_FAIL_IRQ,
			.irq_name = "msg-tx-failed",
		},
		{
			.virq = PMIC_PDPHY_MSG_TX_DISCARD_IRQ,
			.irq_name = "msg-tx-discarded",
		},
		{
			.virq = PMIC_PDPHY_MSG_RX_DISCARD_IRQ,
			.irq_name = "msg-rx-discarded",
		},
	},
	.nr_irqs = 7,
};

static struct pmic_typec_port_resources pm8150b_port_res = {
	.irq_params = {
		{
			.irq_name = "vpd-detect",
			.virq = PMIC_TYPEC_VPD_IRQ,
		},

		{
			.irq_name = "cc-state-change",
			.virq = PMIC_TYPEC_CC_STATE_IRQ,
		},
		{
			.irq_name = "vconn-oc",
			.virq = PMIC_TYPEC_VCONN_OC_IRQ,
		},

		{
			.irq_name = "vbus-change",
			.virq = PMIC_TYPEC_VBUS_IRQ,
		},

		{
			.irq_name = "attach-detach",
			.virq = PMIC_TYPEC_ATTACH_DETACH_IRQ,
		},
		{
			.irq_name = "legacy-cable-detect",
			.virq = PMIC_TYPEC_LEGACY_CABLE_IRQ,
		},

		{
			.irq_name = "try-snk-src-detect",
			.virq = PMIC_TYPEC_TRY_SNK_SRC_IRQ,
		},
	},
	.nr_irqs = 7,
	.reg_fields = &pmic_typec_fields_pm8150b,
};

static struct pmic_typec_port_resources pmi8998_port_res = {
	.irq_params = {
		{
			/*
			 * on pmi8998 this IRQ is used for most things
			 * See TYPE_C_INTRPT_ENB_REG:
			 * - TYPEC_CCOUT_DETACH_INT_EN_BIT
			 * - TYPEC_CCOUT_ATTACH_INT_EN_BIT
			 * - TYPEC_VBUS_ERROR_INT_EN_BIT
			 * - TYPEC_UFP_AUDIOADAPT_INT_EN_BIT
			 * - TYPEC_DEBOUNCE_DONE_INT_EN_BIT
			 * - TYPEC_CCSTATE_CHANGE_INT_EN_BIT
			 * - TYPEC_VBUS_DEASSERT_INT_EN_BIT
			 * - TYPEC_VBUS_ASSERT_INT_EN_BIT
			 *
			 * This irq is probably also used for
			 * PMIC_TYPEC_VBUS_IRQ
			 */
			.irq_name = "type-c-change",
			.virq = PMIC_TYPEC_CC_STATE_IRQ,
		},
		// {
		// 	/* usb-plugin IRQ, shared with charger */
		// 	.irq_name = "usbin-lt-3p6v",
		// 	.virq = PMIC_TYPEC_ATTACH_DETACH_IRQ,
		// 	.irq_flags = 0,
		// },
		// {
		// 	/* usb-plugin IRQ, shared with charger */
		// 	.irq_name = "usb-plugin",
		// 	.virq = PMIC_TYPEC_VBUS_IRQ,
		// 	.irq_flags = IRQF_SHARED,
		// },
	},
	.nr_irqs = 1,
	.reg_fields = &pmic_typec_fields_pmi8998,
};

static struct pmic_typec_resources pm8150b_typec_res = {
	.pdphy_res = &pm8150b_pdphy_res,
	.port_res = &pm8150b_port_res,
};

struct pmic_typec_resources pmi8998_typec_res = {
	.pdphy_res = &pm8150b_pdphy_res,
	.port_res = &pmi8998_port_res,
};

static const struct of_device_id qcom_pmic_typec_table[] = {
	{ .compatible = "qcom,pm8150b-typec", .data = &pm8150b_typec_res },
	{ .compatible = "qcom,pmi8998-typec", .data = &pmi8998_typec_res },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_pmic_typec_table);

static struct platform_driver qcom_pmic_typec_driver = {
	.driver = {
		.name = "qcom,pmic-typec",
		.of_match_table = qcom_pmic_typec_table,
	},
	.probe = qcom_pmic_typec_probe,
	.remove_new = qcom_pmic_typec_remove,
};

module_platform_driver(qcom_pmic_typec_driver);

MODULE_DESCRIPTION("QCOM PMIC USB Type-C Port Manager Driver");
MODULE_LICENSE("GPL");
