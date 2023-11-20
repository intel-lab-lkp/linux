.. SPDX-License-Identifier: GPL-2.0
.. _dtscodingstyle:

=====================================
Devicetree Sources (DTS) Coding Style
=====================================

When writing Devicetree Sources (DTS) please observe below guidelines.  They
should be considered complementary to any rules expressed already in Devicetree
Specification and dtc compiler (including W=1 and W=2 builds).

Individual architectures and sub-architectures can add additional rules, making
the style stricter.

Naming and Valid Characters
---------------------------

1. Node and property names are allowed to use only:

   * lowercase characters: [a-z]
   * digits: [0-9]
   * dash: -

2. Labels are allowed to use only:

   * lowercase characters: [a-z]
   * digits: [0-9]
   * underscore: _

3. Unit addresses should use lowercase hex, without leading zeros (padding).

4. Hex values in properties, e.g. "reg", should use lowercase hex.  The address
   part can be padded with leading zeros.

Example::

	gpi_dma2: dma-controller@800000 {
		compatible = "qcom,sm8550-gpi-dma", "qcom,sm6350-gpi-dma";
		reg = <0x0 0x00800000 0x0 0x60000>;
	}

Order of Nodes
--------------

1. Nodes within any bus, thus using unit addresses for children, shall be
   ordered incrementally by unit address.
   Alternatively for some sub-architectures, nodes of the same type can be
   grouped together (e.g. all I2C controllers one after another even if this
   breaks unit address ordering).

2. Nodes without unit addresses should be ordered alpha-numerically by the node
   name.  For a few types of nodes, they can be ordered by the main property
   (e.g. pin configuration states ordered by value of "pins" property).

3. When extending nodes in the board DTS via &label, the entries should be
   ordered alpha-numerically.

Example::

	// SoC DTSI

	/ {
		cpus {
			// ...
		};

		psci {
			// ...
		};

		soc@ {
			dma: dma-controller@10000 {
				// ...
			};

			clk: clock-controller@80000 {
				// ...
			};
		};
	};

	// Board DTS

	&clk {
		// ...
	};

	&dma {
		// ...
	};


Order of Properties in Device Node
----------------------------------

Following order of properties in device nodes is preferred:

1. compatible
2. reg
3. ranges
4. Standard/common properties (defined by common bindings, e.g. without
   vendor-prefixes)
5. Vendor-specific properties
6. status (if applicable)
7. Child nodes, where each node is preceded with a blank line

The "status" property is by default "okay", thus it can be omitted.

Example::

	// SoC DTSI

	usb_1_hsphy: phy@88e3000 {
		compatible = "qcom,sm8550-snps-eusb2-phy";
		reg = <0x0 0x088e3000 0x0 0x154>;
		#phy-cells = <0>;
		resets = <&gcc GCC_QUSB2PHY_PRIM_BCR>;
		status = "disabled";
	};

	// Board DTS

	&usb_1_hsphy {
		clocks = <&tcsr TCSR_USB2_CLKREF_EN>;
		clock-names = "ref";
		status = "okay";
	};


Indentation
-----------

1. Use indentation according to :ref:`codingstyle`.
2. For arrays spanning across lines, it is preferred to align the continued
   entries with opening < from the first line.
3. Each entry in arrays with multiple cells (e.g. "reg" with two IO addresses)
   should be enclosed in <>.

Example::

	thermal-sensor@c271000 {
		compatible = "qcom,sm8550-tsens", "qcom,tsens-v2";
		reg = <0x0 0x0c271000 0x0 0x1000>,
		      <0x0 0x0c222000 0x0 0x1000>;
	};

Organizing DTSI and DTS
-----------------------

The DTSI and DTS files should be organized in a way representing the common
(and re-usable) parts of the hardware.  Typically this means organizing DTSI
and DTS files into several files:

1. DTSI with contents of the entire SoC (without nodes for hardware not present
   on the SoC).
2. If applicable: DTSI with common or re-usable parts of the hardware (e.g.
   entire System-on-Module).
3. DTS representing the board.

Hardware components which are present on the board should be placed in the
board DTS, not in the SoC or SoM DTSI.  A partial exception is a common
external reference SoC-input clock, which could be coded as a fixed-clock in
the SoC DTSI with its frequency provided by each board DTS.
