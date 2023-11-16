.. SPDX-License-Identifier: GPL-2.0
.. _writingdts:

===================================================
Writing Devicetree Sources (DTS) - DTS Coding Style
===================================================

When writing Devicetree Sources (DTS) please observe below guidelines.  They
should be considered complementary to any rules expressed already in Devicetree
Specification and dtc compiler (including W=1 and W=2 builds).

Individual architectures and sub-architectures can add additional rules, making
the style stricter.

Naming and Valid Characters
---------------------------

1. Node and property names are allowed to use only:

   * lowercase characters:: [a-z]
   * digits:: [0-9]
   * dash:: -

2. Labels are allowed to use only:

   * lowercase characters:: [a-z]
   * digits:: [0-9]
   * underscore:: _

3. Unit addresses should use lowercase hex, without leading zeros (padding).

4. Hex values in properties, e.g. "reg", should use lowercase hex.  Any address
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

2. Nodes without unit addresses should be ordered alpha-numerically.

3. When extending nodes in board DTS via &label, the entries should be ordered
   alpha-numerically.

Example::

	// SoC DTSI

	\ {
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
4. All properties with values
5. Boolean properties
6. status (if applicable)
7. Child nodes

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
2. For arrays spanning across lines, preferred is to align the continued
   entries with opening < from first line.

Example::

	thermal-sensor@c271000 {
		compatible = "qcom,sm8550-tsens", "qcom,tsens-v2";
		reg = <0x0 0x0c271000 0x0 0x1000>,
		      <0x0 0x0c222000 0x0 0x1000>;
	};
