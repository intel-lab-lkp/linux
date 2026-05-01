// SPDX-License-Identifier: GPL-2.0

use kernel::{
    device::Bound,
    pci::{
        self,
        IrqType,
        IrqTypes, //
    },
    prelude::*,
};

mod intr_ctrl;

pub(crate) fn alloc_vector(pdev: &pci::Device<Bound>) -> Result<pci::IrqVector<'_>> {
    let msi_types = IrqTypes::default().with(IrqType::Msi).with(IrqType::MsiX);

    let irq_vectors = match pdev.alloc_irq_vectors(1, 1, msi_types) {
        Ok(vecs) => vecs,
        Err(_) => {
            dev_warn!(pdev.as_ref(), "MSI not available, falling back to INTx\n");
            pdev.alloc_irq_vectors(1, 1, IrqTypes::default().with(IrqType::Intx))?
        }
    };

    Ok(*irq_vectors.start())
}
